#pragma once

#include <entityfy/concurrency.hpp>
#include <entityfy/db_context.hpp>
#include <entityfy/value.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace entityfy
{
namespace detail
{

consteval std::meta::info find_member(std::meta::info type, std::string_view name)
{
    for(const auto member: std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked()))
    {
        if(std::meta::has_identifier(member) && std::meta::identifier_of(member) == name)
        {
            return member;
        }
    }

    return std::meta::info{};
}

consteval std::meta::info concurrency_token_member(std::meta::info type)
{
    for(const auto member: std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked()))
    {
        if(!std::meta::annotations_of_with_type(member, ^^entityfy::concurrency_token_t).empty() ||
           !std::meta::annotations_of_with_type(member, ^^entityfy::xmin_t).empty())
        {
            return member;
        }
    }

    return find_member(type, "version");
}

template <typename T>
consteval std::meta::info concurrency_token_member()
{
    return concurrency_token_member(^^T);
}

// The column a member maps to. The xmin annotation replaces the member name
// with the PostgreSQL system column, matching describe_model().
template <auto Member>
consteval entityfy::annotation_text member_column_name()
{
    if constexpr(!std::meta::annotations_of_with_type(Member, ^^entityfy::xmin_t).empty())
    {
        return entityfy::annotation_text{"xmin"};
    }

    return annotated_column_name<Member>();
}

consteval bool is_ignored_member(std::meta::info member)
{
    return !std::meta::annotations_of_with_type(member, ^^entityfy::ignore_t).empty();
}

// Every primary key member, in declaration order. The annotations are the key
// definition; a model that annotates no key at all falls back to its `id`
// member, matching describe_model(). Composite keys are the same list with
// more than one entry.
consteval std::vector<std::meta::info> primary_key_members(std::meta::info type)
{
    std::vector<std::meta::info> keys;

    for(const auto member: std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked()))
    {
        if(!std::meta::has_identifier(member) || is_ignored_member(member))
        {
            continue;
        }

        if(!std::meta::annotations_of_with_type(member, ^^entityfy::primary_key_t).empty() ||
           !std::meta::annotations_of_with_type(member, ^^entityfy::auto_increment_t).empty())
        {
            keys.push_back(member);
        }
    }

    if(keys.empty())
    {
        if(const auto fallback = find_member(type, "id"); fallback != std::meta::info{})
        {
            keys.push_back(fallback);
        }
    }

    if(keys.empty())
    {
        throw "entityfy: model has no primary key";
    }

    return keys;
}

// The key member of a model whose key the database generates; empty for any
// other model. A generated key always has exactly one integral key column.
consteval std::meta::info generated_key_member(std::meta::info type)
{
    const auto keys = primary_key_members(type);

    return keys.size() == 1 ? keys.front() : std::meta::info{};
}

consteval bool is_primary_key_member(std::meta::info type, std::meta::info member)
{
    for(const auto candidate: primary_key_members(type))
    {
        if(candidate == member)
        {
            return true;
        }
    }

    return false;
}

consteval std::meta::info find_member_of_type(std::meta::info type, std::meta::info desired)
{
    for(const auto member: std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked()))
    {
        if(std::meta::type_of(member) == desired)
        {
            return member;
        }
    }

    return std::meta::info{};
}

// Members the update path compares and writes: every persisted field except
// the primary key and the version column. Resolved once at compile time so a
// save does not rebuild column names inside the per entity loop.
template <typename T>
consteval auto update_members()
{
    std::vector<std::meta::info> members;

    for(const auto member: std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()))
    {
        if(!std::meta::has_identifier(member) || is_ignored_member(member))
        {
            continue;
        }

        if(is_primary_key_member(^^T, member) || member == concurrency_token_member<T>())
        {
            continue;
        }

        members.push_back(member);
    }

    return std::define_static_array(members);
}

template <typename T>
void assign_generated_id(T& entity, std::int64_t value)
{
    constexpr auto member = generated_key_member(^^T);

    if constexpr(member != std::meta::info{})
    {
        using member_type = std::remove_cvref_t<decltype(entity.[:member:])>;

        if constexpr(std::is_integral_v<member_type>)
        {
            entity.[:member:] = static_cast<member_type>(value);
        }
    }
}

template <typename T>
bool is_persisted_column(const entityfy::model_descriptor<T>& model, std::string_view name)
{
    return std::ranges::any_of(model.columns, [&](const entityfy::column_descriptor& column) {
        return column.name == name;
    });
}

template <typename T>
bool is_system_column(const entityfy::model_descriptor<T>& model, std::string_view name)
{
    return std::ranges::any_of(model.columns, [&](const entityfy::column_descriptor& column) {
        return column.name == name && column.flags.system;
    });
}

template <typename T>
[[nodiscard]] concurrency_entry make_concurrency_entry(
    db_context& owner,
    const entityfy::model_descriptor<T>& model,
    const row_key& key,
    std::string_view token_column,
    const sql_value& original_token)
{
    concurrency_entry entry;
    entry.table = model.table_name;
    entry.key = key;
    entry.token_column = std::string{token_column};
    entry.original_token = original_token;
    for(const auto& column: owner.database().inspect_table(model.table_name).columns)
    {
        entry.columns.push_back(column.name);
    }

    // SELECT * keeps the row in the table's own column order, which is what
    // `entry.columns` reports.
    auto filter = entityfy::detail::primary_key_filter(model, key);

    entityfy::statement lookup;
    lookup.sql =
        "SELECT * FROM " + entityfy::detail::quote_identifier(model.table_name) + " WHERE " + filter.sql;
    lookup.binds = std::move(filter.binds);

    auto rows = owner.database().query(lookup);
    if(!rows.empty())
    {
        entry.database_values = std::move(rows.front());
    }

    return entry;
}

// Reads the value of a persisted column from an in memory entity. The column
// is resolved by name through reflection, so the caller never has to pass a
// member pointer.
template <typename Model>
[[nodiscard]] std::optional<sql_value> read_column(const Model& entity, std::string_view column)
{
    std::optional<sql_value> result;

    meta::for_each_persistent_field_reflection<Model, ignore_t>([&]<auto meta_field>() {
        using field_type = meta::field_type_t<Model, meta_field>;

        if constexpr(std::integral<field_type> || std::same_as<field_type, std::string>)
        {
            if(result.has_value())
            {
                return;
            }

            if(std::string_view{annotated_column_name<meta_field>().view()} == column)
            {
                result = entityfy::detail::to_sql_value(entity.[:meta_field:]);
            }
        }
    });

    return result;
}

// Canonical text form of a key value, used to match a foreign key against a
// primary key without caring about the concrete SQL type.
[[nodiscard]] inline std::string canonical_key(const sql_value& value)
{
    if(const auto* number = std::get_if<std::int64_t>(&value))
    {
        return std::to_string(*number);
    }

    if(const auto* number = std::get_if<std::uint64_t>(&value))
    {
        return std::to_string(*number);
    }

    if(const auto* number = std::get_if<double>(&value))
    {
        return std::to_string(static_cast<std::int64_t>(*number));
    }

    if(const auto* text = std::get_if<std::string>(&value))
    {
        return *text;
    }

    return {};
}

// Finds the relation that points from the child model to the parent model.
// The relation is read from the "references" annotation, so no member pointer
// has to be supplied by the caller.
template <typename Child, typename Parent>
[[nodiscard]] const entityfy::relation_descriptor& find_relation(
    const entityfy::model_descriptor<Child>& child_model,
    const entityfy::model_descriptor<Parent>& parent_model)
{
    for(const auto& relation: child_model.relations)
    {
        if(relation.referenced_table == parent_model.table_name)
        {
            return relation;
        }
    }

    throw std::runtime_error{"entityfy: no relation from " + child_model.table_name + " to " +
                             parent_model.table_name};
}

template <typename T>
struct set_state final : saveable
{
    struct tracked
    {
        T            entity;
        T            original;
        entity_state state = entity_state::unchanged;
        // Set by mark_modified(): with auto detection off this is the only
        // signal that an entity changed.
        bool marked_modified = false;
        // Generation of the last save that recorded an undo entry for this
        // item, so the log stays proportional to what the save touched.
        std::uint64_t undo_generation = 0;
    };

    // One pre-save image of a tracked item. Only items the save actually
    // mutates are recorded, so a save with one dirty row stays O(1) in memory
    // instead of copying the whole tracked set.
    struct undo_entry
    {
        std::size_t  index = 0;
        T            entity;
        T            original;
        entity_state state = entity_state::unchanged;
        bool         marked = false;
    };

    std::deque<std::unique_ptr<tracked>>                              items;
    std::unordered_map<row_key, std::size_t, row_key_hash>             identity;
    std::vector<undo_entry>                    undo_;
    // Change tracking hints: a save only visits the indices named here, so the
    // work stays proportional to what changed. Every entry is validated
    // against the item state before use; stale entries are simply skipped.
    std::vector<std::size_t>                   added_indices_;
    std::vector<std::size_t>                   removed_indices_;
    std::vector<std::size_t>                   modified_indices_;
    std::vector<std::size_t>                   backup_added_;
    std::vector<std::size_t>                   backup_removed_;
    std::vector<std::size_t>                   backup_modified_;
    bool                                       needs_compaction_ = false;
    std::uint64_t                              save_generation_ = 0;
    db_context*                                owner_ = nullptr;
    // Resolved once per entity type: key extraction and the update path read
    // the descriptor on every tracked operation.
    entityfy::model_descriptor<T>              model_;
    // True when the database assigns the key (exactly one auto increment key
    // column). Only then does the default key mean "not assigned yet".
    bool                                       generated_key_ = false;
    std::string                                table_name_;
    std::vector<std::string>                   referenced_tables_;

    explicit set_state(db_context& owner)
        : owner_{&owner}
        , model_{entityfy::describe_model<T>(owner.database().target_dialect())}
    {
        table_name_ = model_.table_name;

        for(const auto& relation: model_.relations)
        {
            referenced_tables_.push_back(relation.referenced_table);
        }

        generated_key_ = std::ranges::any_of(model_.columns, [](const entityfy::column_descriptor& column) {
            return column.flags.auto_increment;
        });
    }

    // The entity's key in descriptor order. The model descriptor is the key
    // definition, so this covers single, string and composite keys alike.
    [[nodiscard]] row_key key_of(const T& value) const
    {
        return entityfy::detail::primary_key_of(value, model_);
    }

    // "Unset" is a generated-key concept, not a zero sentinel: only a model
    // whose key the database generates treats the default value as "not
    // assigned yet". An explicit key is always real, including 0 and "".
    [[nodiscard]] bool key_is_unset(const row_key& key) const
    {
        if(!generated_key_ || key.size() != 1)
        {
            return false;
        }

        const auto& part = key.part(0);

        if(const auto* number = std::get_if<std::int64_t>(&part))
        {
            return *number == 0;
        }

        if(const auto* number = std::get_if<std::uint64_t>(&part))
        {
            return *number == 0;
        }

        return false;
    }

    [[nodiscard]] bool key_is_unset(const T& value) const
    {
        return key_is_unset(key_of(value));
    }

    [[nodiscard]] std::string table_name() const override
    {
        return table_name_;
    }

    [[nodiscard]] std::vector<std::string> referenced_tables() const override
    {
        return referenced_tables_;
    }

    void begin_save() override
    {
        undo_.clear();
        ++save_generation_;
        backup_added_ = added_indices_;
        backup_removed_ = removed_indices_;
        backup_modified_ = modified_indices_;
    }

    void commit_save() noexcept override
    {
        undo_.clear();
        backup_added_.clear();
        backup_removed_.clear();
        backup_modified_.clear();
        added_indices_.clear();
        removed_indices_.clear();
        modified_indices_.clear();
        needs_compaction_ = false;
    }

    // Records the current image of an item before the save mutates it.
    void mark_undo(std::size_t index)
    {
        auto& item = *items[index];

        if(item.undo_generation == save_generation_)
        {
            return;
        }

        item.undo_generation = save_generation_;
        undo_.push_back(undo_entry{
            .index = index,
            .entity = item.entity,
            .original = item.original,
            .state = item.state,
            .marked = item.marked_modified,
        });
    }

    void rollback_save() override
    {
        for(auto iterator = undo_.rbegin(); iterator != undo_.rend(); ++iterator)
        {
            auto& item = *items[iterator->index];
            item.entity = iterator->entity;
            item.original = iterator->original;
            item.state = iterator->state;
            item.marked_modified = iterator->marked;
        }

        undo_.clear();
        added_indices_ = backup_added_;
        removed_indices_ = backup_removed_;
        modified_indices_ = backup_modified_;
        backup_added_.clear();
        backup_removed_.clear();
        backup_modified_.clear();
        rebuild_identity();
    }

    save_result delete_removed(db_context& owner) override;
    save_result save(db_context& owner) override;

    template <auto TokenMember>
    void reload_system_token(db_context& owner,
                             T& entity,
                             const entityfy::model_descriptor<T>& model,
                             std::string_view token_column,
                             const row_key& key);

    T& add(T value)
    {
        const row_key key = key_of(value);
        const bool assigned = !key_is_unset(key);
        if(assigned && identity.contains(key))
        {
            throw std::runtime_error{"entityfy: an entity with the same key is already tracked"};
        }

        items.push_back(std::make_unique<tracked>(tracked{.entity = std::move(value)}));
        auto& item = *items.back();
        item.original = item.entity;
        item.state = entity_state::added;

        if(assigned)
        {
            identity[key] = items.size() - 1;
        }

        added_indices_.push_back(items.size() - 1);
        return item.entity;
    }

    T& attach(T value)
    {
        T original = value;
        return attach(std::move(value), std::move(original));
    }

    // Attaches a detached entity together with the snapshot it was loaded
    // with, so a stale concurrency token is preserved and checked on save
    // (EF Core: Attach with original values).
    T& attach(T value, T original)
    {
        const row_key key = key_of(value);

        if(!key_is_unset(key))
        {
            if(const auto found = identity.find(key); found != identity.end())
            {
                return items[found->second]->entity;
            }
        }

        items.push_back(std::make_unique<tracked>(tracked{.entity = std::move(value), .original = std::move(original)}));
        auto& item = *items.back();
        item.state = entity_state::unchanged;

        if(!key_is_unset(key))
        {
            identity[key] = items.size() - 1;
        }

        return item.entity;
    }

    // Reads the row as it exists now without touching the tracked entity.
    [[nodiscard]] std::optional<T> database_values(db_context& owner, const T& value)
    {
        const row_key key = key_of(value);
        if(key_is_unset(key))
        {
            return std::nullopt;
        }

        return owner.database().template find<T>(key);
    }

    // Client wins: the original snapshot becomes the database row while the
    // current (edited) values stay, so the next save matches the token.
    void accept_changes(const T& value, const T& database_row)
    {
        auto& item = mutable_item(value, "accept_changes");

        if(item.state == entity_state::added || item.state == entity_state::removed ||
           item.state == entity_state::detached)
        {
            throw std::runtime_error{"entityfy: accept_changes() requires a saved, tracked entity"};
        }

        item.original = database_row;
        item.marked_modified = false;
    }

    // Per-member variant for merges: only that member's original value moves.
    template <typename Member>
    void accept_changes(const T& value, const T& database_row, Member T::* member)
    {
        auto& item = mutable_item(value, "accept_changes");

        if(item.state == entity_state::added || item.state == entity_state::removed ||
           item.state == entity_state::detached)
        {
            throw std::runtime_error{"entityfy: accept_changes() requires a saved, tracked entity"};
        }

        item.original.*member = database_row.*member;
    }

    void remove(const T& value)
    {
        const auto index = index_of(value);
        if(!index)
        {
            throw std::runtime_error{"entityfy: remove() called for an untracked entity"};
        }

        auto& item = *items[*index];

        // A pending insert that is removed again never reached the database;
        // cancelling it must not issue a DELETE (and cannot affect zero rows).
        if(item.state == entity_state::added)
        {
            item.state = entity_state::detached;
            std::erase(added_indices_, *index);
            forget_index(*index);
            needs_compaction_ = true;
            return;
        }

        if(item.state != entity_state::removed)
        {
            item.state = entity_state::removed;
            removed_indices_.push_back(*index);
        }
    }

    // Manual change tracking: with auto detection off, only entities marked
    // here are compared and written.
    void mark_modified(const T& value)
    {
        const auto index = index_of(value);
        if(!index)
        {
            throw std::runtime_error{"entityfy: mark_modified() called for an untracked entity"};
        }

        auto& item = *items[*index];
        if(item.state == entity_state::unchanged && !item.marked_modified)
        {
            item.marked_modified = true;
            modified_indices_.push_back(*index);
        }
    }

    [[nodiscard]] entity_state state_of(const T& value) const
    {
        const auto index = index_of(value);
        if(!index)
        {
            throw std::runtime_error{"entityfy: state() called for an untracked entity"};
        }

        const auto& item = *items[*index];
        if(item.state != entity_state::unchanged)
        {
            return item.state;
        }

        if(owner_ != nullptr && !owner_->auto_detect())
        {
            return item.marked_modified ? entity_state::modified : entity_state::unchanged;
        }

        const auto& model = model_;
        constexpr auto token_member = concurrency_token_member<T>();
        constexpr bool has_token_member = token_member != std::meta::info{};

        bool modified = false;

        template for (constexpr auto member: std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())))
        {
            if constexpr(!is_ignored_member(member))
            {
                if constexpr(is_primary_key_member(^^T, member))
                {
                    continue;
                }

                if constexpr(has_token_member)
                {
                    if constexpr(member == token_member)
                    {
                        continue;
                    }
                }

                const std::string column{annotated_column_name<member>().view()};
                if(!is_persisted_column(model, column))
                {
                    continue;
                }

                if(!(item.entity.[:member:] == item.original.[:member:]))
                {
                    modified = true;
                }
            }
        }

        return modified ? entity_state::modified : entity_state::unchanged;
    }

    void detach(const T& value)
    {
        const auto index = index_of(value);
        if(!index)
        {
            throw std::runtime_error{"entityfy: detach() called for an untracked entity"};
        }

        items[*index]->state = entity_state::detached;
        forget_index(*index);
        needs_compaction_ = true;
    }

    [[nodiscard]] tracked& mutable_item(const T& value, std::string_view operation)
    {
        const auto index = index_of(value);
        if(!index)
        {
            throw std::runtime_error{"entityfy: " + std::string{operation} + "() called for an untracked entity"};
        }

        return *items[*index];
    }

    void refresh(db_context& owner, T& value)
    {
        const auto index = index_of(value);
        if(!index)
        {
            throw std::runtime_error{"entityfy: refresh() called for an untracked entity"};
        }

        const row_key key = key_of(value);
        if(key_is_unset(key))
        {
            throw std::runtime_error{"entityfy: cannot refresh an unsaved entity"};
        }

        auto fetched = owner.database().template find<T>(key);
        if(!fetched)
        {
            throw std::runtime_error{"entityfy: refresh() could not find the entity"};
        }

        value = *fetched;

        auto& item = *items[*index];
        item.entity = value;
        item.original = value;
        item.state = entity_state::unchanged;
        item.marked_modified = false;
        identity[key] = *index;
        forget_index(*index);
    }

    void forget_index(std::size_t index)
    {
        const auto drop = [index](std::vector<std::size_t>& list) {
            list.erase(std::remove(list.begin(), list.end(), index), list.end());
        };

        drop(added_indices_);
        drop(removed_indices_);
        drop(modified_indices_);
    }

    void rebuild_identity()
    {
        identity.clear();
        for(std::size_t index = 0; index < items.size(); ++index)
        {
            const row_key key = key_of(items[index]->entity);
            if(!key_is_unset(key))
            {
                identity[key] = index;
            }
        }
    }

    [[nodiscard]] std::optional<std::size_t> index_of(const T& value) const
    {
        const row_key key = key_of(value);
        if(!key_is_unset(key))
        {
            if(const auto found = identity.find(key); found != identity.end())
            {
                return found->second;
            }
        }

        for(std::size_t index = 0; index < items.size(); ++index)
        {
            if(&items[index]->entity == &value)
            {
                return index;
            }
        }

        return std::nullopt;
    }
};

template <typename T>
save_result set_state<T>::delete_removed(db_context& owner)
{
    save_result result;
    const auto& model = model_;

    constexpr auto token_member = concurrency_token_member<T>();
    constexpr bool has_token_member = token_member != std::meta::info{};

    std::string token_column;
    bool has_token = false;

    if constexpr(has_token_member)
    {
        token_column = std::string{member_column_name<token_member>().view()};
        has_token = is_persisted_column(model, token_column);
    }

    for(const std::size_t index: removed_indices_)
    {
        auto& item = *items[index];
        if(item.state != entity_state::removed)
        {
            continue;
        }

        const row_key key = key_of(item.entity);
        if(!key_is_unset(key))
        {
            sql_value original_token{};

            if constexpr(has_token_member)
            {
                if(has_token)
                {
                    original_token = entityfy::detail::to_sql_value(item.original.[:token_member:]);
                }
            }

            auto command = has_token
                               ? entityfy::delete_by_key_statement(model, key, token_column, original_token)
                               : entityfy::delete_by_key_statement(model, key);
            owner.log(command.sql);

            const auto removed = owner.database().execute(command);
            if(removed.rows_affected == 0)
            {
                throw entityfy::concurrency_error{
                    has_token ? "entityfy: optimistic concurrency conflict for delete of key " + key.to_string()
                              : "entityfy: delete affected zero rows for key " + key.to_string(),
                    {make_concurrency_entry(owner, model, key, token_column, original_token)}};
            }

            result.removed += static_cast<std::size_t>(removed.rows_affected);
        }

        mark_undo(index);
        item.state = entity_state::detached;
        needs_compaction_ = true;
    }

    return result;
}

template <typename T>
template <auto TokenMember>
void set_state<T>::reload_system_token(db_context& owner,
                                       T& entity,
                                       const entityfy::model_descriptor<T>& model,
                                       std::string_view token_column,
                                       const row_key& key)
{
    auto filter = entityfy::detail::primary_key_filter(model, key);

    entityfy::statement lookup;
    lookup.sql = "SELECT " + entityfy::detail::quote_identifier(token_column) + " FROM " +
                 entityfy::detail::quote_identifier(model.table_name) + " WHERE " + filter.sql;
    lookup.binds = std::move(filter.binds);

    const auto rows = owner.database().query(lookup);
    if(rows.empty() || rows.front().empty())
    {
        return;
    }

    entityfy::detail::assign_sql_value(rows.front().front(), entity.[:TokenMember:]);
}

template <typename T>
save_result set_state<T>::save(db_context& owner)
{
    save_result result;

    const auto& model = model_;
    const std::string table = entityfy::detail::quote_identifier(model.table_name);

    constexpr auto token_member = concurrency_token_member<T>();
    constexpr bool has_token_member = token_member != std::meta::info{};

    // Resolved once: an aliased [[= entityfy::column{"..."}]] or a
    // [[= entityfy::concurrency_token]] member must drive the SET and WHERE
    // clauses. A system token ([[= entityfy::xmin]]) is assigned by the server,
    // so it is only compared and then read back instead of incremented.
    std::string token_column;
    bool has_token = false;
    bool system_token = false;

    if constexpr(has_token_member)
    {
        token_column = std::string{member_column_name<token_member>().view()};
        has_token = is_persisted_column(model, token_column);
        system_token = has_token && is_system_column(model, token_column);
    }

    const auto generated_column = [&](std::string_view name) {
        for(const auto& column: model.columns)
        {
            if(column.name == name)
            {
                return column.generated_on_insert;
            }
        }

        return false;
    };

    std::vector<std::string> insert_columns;

    template for (constexpr auto member: std::define_static_array(
                      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())))
    {
        const std::string column{member_column_name<member>()};
        if(!is_persisted_column(model, column) || generated_column(column) || is_system_column(model, column))
        {
            continue;
        }

        insert_columns.push_back(column);
    }

    const bool auto_detect = owner.auto_detect();

    std::vector<std::size_t> added_indices;
    added_indices.reserve(added_indices_.size());

    for(const std::size_t index: added_indices_)
    {
        if(items[index]->state == entity_state::added)
        {
            added_indices.push_back(index);
        }
    }

    for(const std::size_t index: added_indices)
    {
        if(generated_key_ && !key_is_unset(items[index]->entity))
        {
            throw std::runtime_error{"entityfy: explicit primary key is not allowed for an auto-increment model"};
        }
    }

    for(const std::size_t index: removed_indices_)
    {
        auto& item = *items[index];
        if(item.state == entity_state::removed)
        {
            mark_undo(index);
            item.state = entity_state::detached;
            needs_compaction_ = true;
        }
    }

    if(!added_indices.empty() && !insert_columns.empty() && !generated_key_)
    {
        std::string prefix = "INSERT INTO " + table + " (";
        for(std::size_t index = 0; index < insert_columns.size(); ++index)
        {
            if(index != 0)
            {
                prefix += ", ";
            }

            prefix += entityfy::detail::quote_identifier(insert_columns[index]);
        }

        prefix += ") VALUES ";

        const std::size_t max_rows =
            std::max<std::size_t>(1, owner.batch_parameter_limit() / insert_columns.size());

        for(std::size_t chunk = 0; chunk < added_indices.size(); chunk += max_rows)
        {
            const std::size_t chunk_end = std::min(added_indices.size(), chunk + max_rows);
            std::string sql = prefix;
            std::vector<entityfy::sql_value> binds;

            for(std::size_t row = chunk; row < chunk_end; ++row)
            {
                if(row != chunk)
                {
                    sql += ", ";
                }

                sql += "(";
                bool first_column = true;
                auto& entity = items[added_indices[row]]->entity;

                template for (constexpr auto member: std::define_static_array(
                                  std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())))
                {
                    if constexpr(!is_ignored_member(member))
                    {
                        const std::string column{member_column_name<member>()};

                        if(std::ranges::find(insert_columns, column) == insert_columns.end())
                        {
                            continue;
                        }

                        if(!first_column)
                        {
                            sql += ", ";
                        }

                        sql += "?";
                        binds.push_back(entityfy::detail::to_sql_value(entity.[:member:]));
                        first_column = false;
                    }
                }

                sql += ")";
            }

            owner.log(sql);

            const auto executed =
                owner.database().execute(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});

            const std::size_t expected = chunk_end - chunk;
            if(executed.rows_affected != expected)
            {
                throw std::runtime_error{"entityfy: batched insert affected an unexpected number of rows"};
            }

            result.inserted += expected;

            for(std::size_t row = chunk; row < chunk_end; ++row)
            {
                const std::size_t index = added_indices[row];
                auto& item = *items[index];
                mark_undo(index);

                if constexpr(has_token_member)
                {
                    if(system_token)
                    {
                        reload_system_token<token_member>(
                            owner, item.entity, model, token_column, key_of(item.entity));
                    }
                }

                item.original = item.entity;
                item.state = entity_state::unchanged;

                const row_key key = key_of(item.entity);
                if(!key_is_unset(key))
                {
                    identity[key] = index;
                }
            }
        }
    }

    // Auto detection has to visit everything; manual mode visits the entities
    // that were marked (plus the pending inserts), so a one-row save does not
    // walk the tracked set.
    std::vector<std::size_t> pending;
    if(auto_detect)
    {
        pending.resize(items.size());
        for(std::size_t index = 0; index < items.size(); ++index)
        {
            pending[index] = index;
        }
    }
    else
    {
        pending.reserve(added_indices.size() + modified_indices_.size());

        for(const std::size_t index: added_indices)
        {
            if(items[index]->state == entity_state::added)
            {
                pending.push_back(index);
            }
        }

        for(const std::size_t index: modified_indices_)
        {
            if(items[index]->state == entity_state::unchanged && items[index]->marked_modified)
            {
                pending.push_back(index);
            }
        }
    }

    for(const std::size_t index: pending)
    {
        auto& item = *items[index];

        if(item.state == entity_state::detached)
        {
            continue;
        }

        if(item.state == entity_state::added)
        {
            if(generated_key_ && !key_is_unset(item.entity))
            {
                throw std::runtime_error{"entityfy: explicit primary key is not allowed for an auto-increment model"};
            }

            mark_undo(index);

            auto command = entityfy::insert_statement(item.entity, model);
            owner.log(command.sql);

            const auto inserted = owner.database().execute(command);
            result.inserted += static_cast<std::size_t>(inserted.rows_affected);

            if(generated_key_ && key_is_unset(item.entity))
            {
                std::int64_t generated = inserted.last_insert_id;

                if(generated == 0 && owner.database().target_dialect() == entityfy::dialect::postgresql)
                {
                    const auto rows = owner.database().query(entityfy::statement{.sql = "SELECT lastval()"});
                    if(!rows.empty() && !rows.front().empty())
                    {
                        const auto& value = rows.front().front();
                        if(const auto* number = std::get_if<std::int64_t>(&value))
                        {
                            generated = *number;
                        }
                        else if(const auto* text = std::get_if<std::string>(&value))
                        {
                            generated = std::stoll(*text);
                        }
                    }
                }

                if(generated == 0)
                {
                    throw std::runtime_error{"entityfy: insert did not return a generated primary key"};
                }

                assign_generated_id(item.entity, generated);
            }

            if constexpr(has_token_member)
            {
                if(system_token)
                {
                    reload_system_token<token_member>(
                        owner, item.entity, model, token_column, key_of(item.entity));
                }
            }

            item.original = item.entity;
            item.state = entity_state::unchanged;

            const row_key key = key_of(item.entity);
            if(!key_is_unset(key))
            {
                identity[key] = index;
            }

            continue;
        }

        if(item.state == entity_state::removed)
        {
            const row_key key = key_of(item.entity);
            if(!key_is_unset(key))
            {
                auto command = entityfy::delete_by_key_statement(model, key);
                owner.log(command.sql);
                result.removed += static_cast<std::size_t>(owner.database().execute(command).rows_affected);
            }

            continue;
        }

        // Manual mode: an entity that was not marked is never compared.
        if(!auto_detect && !item.marked_modified)
        {
            continue;
        }

        // Allocated only once a field actually differs, so unchanged entities
        // cost two comparisons and nothing else.
        std::string sql;
        std::vector<entityfy::sql_value> binds;
        bool first = true;

        template for (constexpr auto member: update_members<T>())
        {
            const auto& current = item.entity.[:member:];
            const auto& original = item.original.[:member:];

            if(current == original)
            {
                continue;
            }

            if(first)
            {
                sql = "UPDATE " + table + " SET ";
                first = false;
            }
            else
            {
                sql += ", ";
            }

            sql += entityfy::detail::quote_identifier(annotated_column_name<member>().view()) + " = ?";
            binds.push_back(entityfy::detail::to_sql_value(current));
        }

        if(first)
        {
            mark_undo(index);
            item.original = item.entity;
            item.marked_modified = false;
            continue;
        }

        mark_undo(index);

        if constexpr(has_token_member)
        {
            if(has_token && !system_token)
            {
                sql += ", " + entityfy::detail::quote_identifier(token_column) + " = " +
                       entityfy::detail::quote_identifier(token_column) + " + 1";
            }
        }

        const row_key key = key_of(item.entity);
        const auto key_filter = entityfy::detail::primary_key_filter(model, key);
        sql += " WHERE " + key_filter.sql;
        binds.insert(binds.end(), key_filter.binds.begin(), key_filter.binds.end());

        sql_value original_token{};

        if constexpr(has_token_member)
        {
            if(has_token)
            {
                original_token = entityfy::detail::to_sql_value(item.original.[:token_member:]);
                sql += " AND " + entityfy::detail::quote_identifier(token_column) + " = ?";
                binds.push_back(original_token);
            }
        }

        owner.log(sql);

        const auto updated = owner.database().execute(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});
        result.updated += static_cast<std::size_t>(updated.rows_affected);

        if(updated.rows_affected == 0)
        {
            throw entityfy::concurrency_error{
                has_token ? "entityfy: optimistic concurrency conflict for key " + key.to_string()
                          : "entityfy: update affected zero rows for key " + key.to_string(),
                {make_concurrency_entry(owner, model, key, token_column, original_token)}};
        }

        if constexpr(has_token_member)
        {
            if(has_token)
            {
                using token_type = std::remove_cvref_t<decltype(item.entity.[:token_member:])>;
                static_assert(std::is_integral_v<token_type>,
                              "entityfy: the concurrency token must be an integral type");

                if(system_token)
                {
                    // The server assigned a new xmin; read it so the next save
                    // compares against the current value.
                    reload_system_token<token_member>(
                        owner, item.entity, model, token_column, key_of(item.entity));
                }
                else
                {
                    item.entity.[:token_member:] =
                        static_cast<token_type>(item.entity.[:token_member:] + static_cast<token_type>(1));
                }
            }
        }

        item.original = item.entity;
        item.marked_modified = false;
    }

    const bool has_removed = needs_compaction_;

    if(has_removed)
    {
        std::deque<std::unique_ptr<tracked>> kept;
        for(auto& holder: items)
        {
            if(holder->state != entity_state::removed && holder->state != entity_state::detached)
            {
                kept.push_back(std::move(holder));
            }
        }

        items.swap(kept);
        needs_compaction_ = false;
        // Compaction shifts the indices, so the identity map has to be rebuilt.
        // Without it the map is still valid and a save stays O(changed).
        rebuild_identity();
    }

    // The lists name indices that the compaction may have shifted; after a
    // successful save everything they tracked has been written anyway.
    for(const std::size_t index: modified_indices_)
    {
        if(index < items.size())
        {
            items[index]->marked_modified = false;
        }
    }

    added_indices_.clear();
    removed_indices_.clear();
    modified_indices_.clear();

    return result;
}

} // namespace detail

template <typename T>
class db_query_builder;

template <typename T>
class db_untracked_query_builder;

// The db_set equivalent of EF Core's DbSet: a typed handle over one entity
// type. It is a lazy view, so it can be declared as a member of a user
// defined context:
//
//     struct app_context : entityfy::db_context
//     {
//         using entityfy::db_context::db_context;
//         entityfy::db_set<user> users{*this};
//     };
//
//     ctx.users.add(user{...});
//     ctx.save_changes();
//
// The set resolves its table, columns, primary key and relations through
// reflection, so no schema information is repeated by the caller.
template <typename T>
class db_set
{
public:
    explicit db_set(db_context& owner)
        : owner_{&owner}
    {
        owner.register_model<T>();
    }

    // Tracks a new entity; it is written to the database on the next save.
    T& add(T value)
    {
        return state()->add(std::move(value));
    }

    // Tracks several new entities at once.
    void add_range(const std::vector<T>& values)
    {
        for(const auto& value: values)
        {
            add(value);
        }
    }

    // Tracks an entity without marking it as new.
    T& attach(T value)
    {
        return state()->attach(std::move(value));
    }

    // Attaches an entity together with the snapshot it was loaded with, so a
    // stale concurrency token survives the round trip.
    T& attach(T value, T original)
    {
        return state()->attach(std::move(value), std::move(original));
    }

    // Marks a tracked entity as removed.
    void remove(const T& value)
    {
        state()->remove(value);
    }

    [[nodiscard]] entity_state state(const T& value) const
    {
        return state()->state_of(value);
    }

    void detach(const T& value)
    {
        state()->detach(value);
    }

    void refresh(T& value)
    {
        state()->refresh(*owner_, value);
    }

    // Reads the current database row without touching the tracked entity.
    [[nodiscard]] std::optional<T> database_values(const T& value)
    {
        return state()->database_values(*owner_, value);
    }

    // Client wins: keep the edited values, take the database row as the new
    // original snapshot so the next save matches the concurrency token.
    void accept_changes(const T& value, const T& database_row)
    {
        state()->accept_changes(value, database_row);
    }

    // Merge: only the named member's original value moves to the database row.
    template <typename Member>
    void accept_changes(const T& value, const T& database_row, Member T::* member)
    {
        state()->accept_changes(value, database_row, member);
    }

    // Manual change tracking: marks a tracked entity as modified. With
    // db_context::set_auto_detect(false) this is the only signal save_changes()
    // uses; with auto detection on it is redundant but harmless.
    void mark_modified(T& value)
    {
        state()->mark_modified(value);
    }

    // Loads an entity by primary key. Returns nullptr when it does not exist.
    [[nodiscard]] T* find(const row_key& key)
    {
        auto tracked_state = state();

        if(const auto found = tracked_state->identity.find(key); found != tracked_state->identity.end())
        {
            return &tracked_state->items[found->second]->entity;
        }

        const auto model = entityfy::describe_model<T>(owner_->database().target_dialect());
        auto command = entityfy::select_by_key_statement(model, key);
        owner_->log(command.sql);

        T* found_entity = nullptr;
        owner_->database().query_each(command, [&](const entityfy::detail::row& current) {
            found_entity = &tracked_state->attach(entityfy::detail::materialize_row<T>(current));
            return false;
        });

        return found_entity;
    }

    // Single-value convenience for a one-column key (int64, string, ...); it
    // builds the one-part row_key the general lookup expects.
    template <typename Id>
        requires(!std::same_as<std::remove_cvref_t<Id>, row_key>)
    [[nodiscard]] T* find(Id&& id)
    {
        return find(entityfy::detail::single_part_key(std::forward<Id>(id)));
    }

    // Loads every row of the table and tracks it.
    [[nodiscard]] std::vector<T*> to_list(std::size_t limit = 0)
    {
        auto tracked_state = state();

        entityfy::query_options<T> options;
        if(limit != 0)
        {
            options.limit = limit;
        }

        const auto model = entityfy::describe_model<T>(owner_->database().target_dialect());
        auto command = entityfy::select_statement(model, options);
        owner_->log(command.sql);

        std::vector<T*> output;
        owner_->database().query_each(command, [&](const entityfy::detail::row& current) {
            output.push_back(&tracked_state->attach(entityfy::detail::materialize_row<T>(current)));
            return true;
        });

        return output;
    }

    // Number of entities currently tracked by this set (EF: DbSet.Local.count).
    // The database count is query().count().
    [[nodiscard]] std::size_t size() const noexcept
    {
        return state()->items.size();
    }

    [[nodiscard]] db_query_builder<T> query();

    // Full-entity reads that are materialized but never attached: the EF Core
    // AsNoTracking() path. Rows come back by value, so they pay for neither the
    // `original` snapshot nor an identity map entry. The flip side is that the
    // values are the caller's copies - changing them does not reach the
    // database, and saving them is not an error (see docs/context.md).
    [[nodiscard]] db_untracked_query_builder<T> as_no_tracking();

    // Loads the rows produced by a raw SQL statement and tracks them.
    [[nodiscard]] std::vector<T*> from_sql(std::string_view sql)
    {
        auto tracked_state = state();
        owner_->log(sql);

        std::vector<T*> output;
        owner_->database().query_each(entityfy::statement{.sql = std::string{sql}},
                                       [&](const entityfy::detail::row& current) {
                                           output.push_back(
                                               &tracked_state->attach(entityfy::detail::materialize_row<T>(current)));
                                           return true;
                                       });

        return output;
    }

    template <typename Other, typename Member, typename OtherMember>
    [[nodiscard]] std::vector<std::pair<T*, Other*>> join(Member T::* left, OtherMember Other::* right)
    {
        const auto dialect = owner_->database().target_dialect();
        const auto left_model = entityfy::describe_model<T>(dialect);
        const auto right_model = entityfy::describe_model<Other>(dialect);

        const auto quote = [](std::string_view text) {
            return entityfy::detail::quote_identifier(text);
        };

        std::string sql = "SELECT ";

        for(std::size_t index = 0; index < left_model.columns.size(); ++index)
        {
            if(index != 0)
            {
                sql += ", ";
            }

            sql += "\"a\"." + quote(left_model.columns[index].name);
        }

        for(const auto& column: right_model.columns)
        {
            sql += ", \"b\"." + quote(column.name);
        }

        sql += " FROM " + quote(left_model.table_name) + " AS \"a\" JOIN " + quote(right_model.table_name) +
               " AS \"b\" ON \"a\"." + quote(entityfy::column_name(left)) + " = \"b\"." +
               quote(entityfy::column_name(right));

        owner_->log(sql);

        auto other_set = owner_->template set<Other>();

        const auto split = left_model.columns.size();

        std::vector<std::pair<T*, Other*>> output;
        owner_->database().query_each(entityfy::statement{.sql = std::move(sql)}, [&](const entityfy::detail::row& row) {
            if(row.size() < split)
            {
                throw std::runtime_error{"entityfy: join row has fewer columns than the left model"};
            }

            std::vector<entityfy::sql_value> left_row(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(split));
            std::vector<entityfy::sql_value> right_row(row.begin() + static_cast<std::ptrdiff_t>(split), row.end());

            auto left_value = entityfy::detail::materialize_row<T>(left_row);
            auto right_value = entityfy::detail::materialize_row<Other>(right_row);

            output.emplace_back(&state()->attach(std::move(left_value)),
                                &other_set.attach(std::move(right_value)));
            return true;
        });

        return output;
    }

    template <typename Other, typename Third, typename Member, typename OtherMember,
              typename ThirdMember, typename Member2>
    [[nodiscard]] std::vector<std::tuple<T*, Other*, Third*>> join(
        Member T::* left,
        OtherMember Other::* right,
        ThirdMember Third::* third,
        Member2 T::* left2)
    {
        const auto dialect = owner_->database().target_dialect();
        const auto left_model = entityfy::describe_model<T>(dialect);
        const auto middle_model = entityfy::describe_model<Other>(dialect);
        const auto third_model = entityfy::describe_model<Third>(dialect);

        const auto quote = [](std::string_view text) {
            return entityfy::detail::quote_identifier(text);
        };

        std::string sql = "SELECT ";

        for(std::size_t index = 0; index < left_model.columns.size(); ++index)
        {
            if(index != 0)
            {
                sql += ", ";
            }

            sql += "\"a\"." + quote(left_model.columns[index].name);
        }

        for(const auto& column: middle_model.columns)
        {
            sql += ", \"b\"." + quote(column.name);
        }

        for(const auto& column: third_model.columns)
        {
            sql += ", \"c\"." + quote(column.name);
        }

        sql += " FROM " + quote(left_model.table_name) + " AS \"a\" JOIN " +
               quote(middle_model.table_name) + " AS \"b\" ON \"a\"." + quote(entityfy::column_name(left)) + " = \"b\"." +
               quote(entityfy::column_name(right)) + " JOIN " + quote(third_model.table_name) +
               " AS \"c\" ON \"c\"." + quote(entityfy::column_name(third)) + " = \"a\"." +
               quote(entityfy::column_name(left2));

        owner_->log(sql);

        auto middle_set = owner_->template set<Other>();
        auto right_set = owner_->template set<Third>();

        const auto first_split = left_model.columns.size();
        const auto second_split = first_split + middle_model.columns.size();

        std::vector<std::tuple<T*, Other*, Third*>> output;
        owner_->database().query_each(entityfy::statement{.sql = std::move(sql)}, [&](const entityfy::detail::row& row) {
            if(row.size() < second_split)
            {
                throw std::runtime_error{"entityfy: three-way join row has fewer columns than expected"};
            }

            std::vector<entityfy::sql_value> left_row(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(first_split));
            std::vector<entityfy::sql_value> middle_row(
                row.begin() + static_cast<std::ptrdiff_t>(first_split), row.begin() + static_cast<std::ptrdiff_t>(second_split));
            std::vector<entityfy::sql_value> right_row(row.begin() + static_cast<std::ptrdiff_t>(second_split), row.end());

            auto left_value = entityfy::detail::materialize_row<T>(left_row);
            auto middle_value = entityfy::detail::materialize_row<Other>(middle_row);
            auto right_value = entityfy::detail::materialize_row<Third>(right_row);

            output.emplace_back(&state()->attach(std::move(left_value)),
                                &middle_set.attach(std::move(middle_value)),
                                &right_set.attach(std::move(right_value)));
            return true;
        });

        return output;
    }

    // Eager loading of the children of the given parents. The foreign key is
    // resolved from the "references" annotation, so the caller does not pass a
    // member pointer.
    template <typename Child>
    [[nodiscard]] std::vector<std::pair<T*, std::vector<Child*>>> include(const std::vector<T*>& parents)
    {
        std::vector<std::pair<T*, std::vector<Child*>>> output;
        output.reserve(parents.size());

        if(parents.empty())
        {
            return output;
        }

        const auto dialect = owner_->database().target_dialect();
        const auto parent_model = entityfy::describe_model<T>(dialect);
        const auto child_model = entityfy::describe_model<Child>(dialect);
        const auto& relation = entityfy::detail::find_relation<Child, T>(child_model, parent_model);

        if(entityfy::primary_key_columns(parent_model).size() > 1)
        {
            throw std::logic_error{"entityfy: composite foreign keys are not supported yet (P1.5): relation from " +
                                   child_model.table_name + " to " + parent_model.table_name};
        }

        const auto parent_key_column = entityfy::primary_key_column(parent_model).name;
        if(relation.referenced_column != parent_key_column)
        {
            throw std::runtime_error{"entityfy: relation from " + child_model.table_name + " to " +
                                     parent_model.table_name + " does not point at the primary key"};
        }

        std::vector<entityfy::sql_value> parent_keys;
        parent_keys.reserve(parents.size());

        for(const auto* parent: parents)
        {
            parent_keys.push_back(
                entityfy::detail::primary_key_value(*parent, parent_model)
            );
        }

        std::string sql = "SELECT ";

        for(std::size_t index = 0; index < child_model.columns.size(); ++index)
        {
            if(index != 0)
            {
                sql += ", ";
            }

            sql += entityfy::detail::quote_identifier(child_model.columns[index].name);
        }

        sql += " FROM " + entityfy::detail::quote_identifier(child_model.table_name) + " WHERE " +
               entityfy::detail::quote_identifier(relation.local_column) + " IN (";

        for(std::size_t index = 0; index < parent_keys.size(); ++index)
        {
            if(index != 0)
            {
                sql += ", ";
            }

            sql += "?";
        }

        sql += ")";

        owner_->log(sql);

        std::unordered_map<row_key, std::vector<Child*>, row_key_hash> grouped;
        auto child_set = owner_->template set<Child>();

        owner_->database().query_each(
            entityfy::statement{.sql = std::move(sql), .binds = std::move(parent_keys)},
            [&](const entityfy::detail::row& row) {
                auto child = entityfy::detail::materialize_row<Child>(row);

                const auto key = entityfy::detail::read_column(child, relation.local_column);
                if(!key)
                {
                    return true;
                }

                auto& stored = child_set.attach(std::move(child));
                grouped[entityfy::detail::single_part_key(*key)].push_back(&stored);
                return true;
            }
        );

        for(T* parent: parents)
        {
            const auto key =
                entityfy::detail::single_part_key(entityfy::detail::primary_key_value(*parent, parent_model));
            auto bucket = grouped[key];

            constexpr auto pointer_navigation = detail::find_member_of_type(^^T, ^^std::vector<Child*>);
            constexpr auto value_navigation = detail::find_member_of_type(^^T, ^^std::vector<Child>);

            if constexpr(pointer_navigation != std::meta::info{})
            {
                (*parent).[:pointer_navigation:] = bucket;
            }
            else if constexpr(value_navigation != std::meta::info{})
            {
                std::vector<Child> values;
                values.reserve(bucket.size());

                for(Child* child: bucket)
                {
                    values.push_back(*child);
                }

                (*parent).[:value_navigation:] = std::move(values);
            }

            output.emplace_back(parent, std::move(bucket));
        }

        return output;
    }

    // Eager loading of the parent of the given children. The foreign key is
    // resolved from the "references" annotation.
    template <typename Parent>
    [[nodiscard]] std::vector<std::pair<T*, Parent*>> include_parent(const std::vector<T*>& children)
    {
        std::vector<std::pair<T*, Parent*>> output;
        output.reserve(children.size());

        if(children.empty())
        {
            return output;
        }

        const auto dialect = owner_->database().target_dialect();
        const auto child_model = entityfy::describe_model<T>(dialect);
        const auto parent_model = entityfy::describe_model<Parent>(dialect);
        const auto& relation = entityfy::detail::find_relation<T, Parent>(child_model, parent_model);

        if(entityfy::primary_key_columns(parent_model).size() > 1)
        {
            throw std::logic_error{"entityfy: composite foreign keys are not supported yet (P1.5): relation from " +
                                   child_model.table_name + " to " + parent_model.table_name};
        }

        const auto parent_key_column = entityfy::primary_key_column(parent_model).name;
        if(relation.referenced_column != parent_key_column)
        {
            throw std::runtime_error{"entityfy: relation from " + child_model.table_name + " to " +
                                     parent_model.table_name + " does not point at the primary key"};
        }

        std::vector<row_key> keys;
        std::vector<entityfy::sql_value> binds;

        for(auto* child: children)
        {
            const auto key = entityfy::detail::read_column(*child, relation.local_column);
            if(!key)
            {
                continue;
            }

            const auto child_key = entityfy::detail::single_part_key(*key);
            if(std::ranges::find(keys, child_key) == keys.end())
            {
                keys.push_back(child_key);
                binds.push_back(*key);
            }
        }

        if(binds.empty())
        {
            for(auto* child: children)
            {
                output.emplace_back(child, nullptr);
            }

            return output;
        }

        std::string sql = "SELECT ";

        for(std::size_t index = 0; index < parent_model.columns.size(); ++index)
        {
            if(index != 0)
            {
                sql += ", ";
            }

            sql += entityfy::detail::quote_identifier(parent_model.columns[index].name);
        }

        sql += " FROM " + entityfy::detail::quote_identifier(parent_model.table_name) + " WHERE " +
               entityfy::detail::quote_identifier(parent_key_column) + " IN (";

        for(std::size_t index = 0; index < binds.size(); ++index)
        {
            if(index != 0)
            {
                sql += ", ";
            }

            sql += "?";
        }

        sql += ")";

        owner_->log(sql);

        std::unordered_map<row_key, Parent*, row_key_hash> loaded;
        auto parent_set = owner_->template set<Parent>();

        owner_->database().query_each(
            entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)},
            [&](const entityfy::detail::row& row) {
                auto value = entityfy::detail::materialize_row<Parent>(row);
                const auto key = entityfy::detail::primary_key_value(value, parent_model);
                Parent& stored = parent_set.attach(std::move(value));
                loaded[entityfy::detail::single_part_key(key)] = &stored;
                return true;
            }
        );

        for(auto* child: children)
        {
            const auto key = entityfy::detail::read_column(*child, relation.local_column);
            if(!key)
            {
                output.emplace_back(child, nullptr);
                continue;
            }

            const auto found = loaded.find(entityfy::detail::single_part_key(*key));
            output.emplace_back(child, found == loaded.end() ? nullptr : found->second);
        }

        return output;
    }

private:
    [[nodiscard]] std::shared_ptr<detail::set_state<T>> state() const
    {
        return owner_->template state_for<T>();
    }

    db_context* owner_;
};

// Owning, read-only result of a no-tracking read. Handing out const references
// makes "edit the entity and expect it to be saved" a compile error instead of
// EF Core's silent no-op; taking a copy is still allowed and is then just a
// copy, as in any other value type.
template <typename T>
class no_tracking_result
{
public:
    explicit no_tracking_result(std::vector<T> rows)
        : rows_{std::move(rows)}
    {
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return rows_.size();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return rows_.empty();
    }

    [[nodiscard]] const T& operator[](std::size_t index) const noexcept
    {
        return rows_[index];
    }

    [[nodiscard]] const T& front() const noexcept
    {
        return rows_.front();
    }

    [[nodiscard]] const T& back() const noexcept
    {
        return rows_.back();
    }

    [[nodiscard]] const T* begin() const noexcept
    {
        return rows_.data();
    }

    [[nodiscard]] const T* end() const noexcept
    {
        return rows_.data() + rows_.size();
    }

    [[nodiscard]] std::span<const T> rows() const noexcept
    {
        return rows_;
    }

private:
    std::vector<T> rows_;
};

template <typename T, typename Child>
class db_untracked_include_builder;

// The value-returning counterpart of db_query_builder: same filters and paging,
// but every row is materialized into a std::vector<T> and never attached to the
// tracker. Reads only; there is no save path from here.
template <typename T>
class db_untracked_query_builder
{
public:
    explicit db_untracked_query_builder(db_context& owner)
        : owner_{&owner}
    {
    }

    [[nodiscard]] db_untracked_query_builder where(entityfy::condition filter) const
    {
        auto copy = *this;
        copy.options_.filter = std::move(filter);
        return copy;
    }

    template <typename Member>
    [[nodiscard]] db_untracked_query_builder order_by(
        Member T::* member,
        entityfy::sort_direction direction = entityfy::sort_direction::ascending) const
    {
        auto copy = *this;
        copy.options_.orderings.push_back(entityfy::order_by(member, direction));
        return copy;
    }

    [[nodiscard]] db_untracked_query_builder take(std::uint64_t count) const
    {
        auto copy = *this;
        copy.options_.limit = count;
        return copy;
    }

    [[nodiscard]] db_untracked_query_builder skip(std::uint64_t offset) const
    {
        auto copy = *this;
        copy.options_.offset = offset;
        return copy;
    }

    [[nodiscard]] no_tracking_result<T> to_list() const
    {
        const auto model = entityfy::describe_model<T>(owner_->database().target_dialect());
        const auto command = entityfy::select_statement(model, options_);

        owner_->log(command.sql);

        std::vector<T> output;
        owner_->database().query_each(command, [&](const entityfy::detail::row& current) {
            output.push_back(entityfy::detail::materialize_row<T>(current));
            return true;
        });

        return no_tracking_result<T>{std::move(output)};
    }

    // A copy owned by the caller: mutating it never reaches the database.
    [[nodiscard]] std::optional<T> first() const
    {
        auto copy = *this;
        copy.options_.limit = 1;

        const auto rows = copy.to_list();
        if(rows.empty())
        {
            return std::nullopt;
        }

        return rows.front();
    }

    [[nodiscard]] std::uint64_t count() const
    {
        return owner_->database().template count<T>(options_);
    }

    // Eager loading on a no-tracking read: children are returned as values, so
    // no pointer into the result can dangle.
    template <typename Child>
    [[nodiscard]] std::vector<std::pair<T, std::vector<Child>>> to_list_with_children() const
    {
        const auto parent_rows = to_list();
        std::vector<std::pair<T, std::vector<Child>>> output;
        output.reserve(parent_rows.size());

        const auto dialect = owner_->database().target_dialect();
        const auto parent_model = entityfy::describe_model<T>(dialect);
        const auto child_model = entityfy::describe_model<Child>(dialect);
        const auto& relation = entityfy::detail::find_relation<Child, T>(child_model, parent_model);

        std::vector<entityfy::sql_value> parent_keys;
        parent_keys.reserve(parent_rows.size());

        for(const auto& parent: parent_rows)
        {
            parent_keys.push_back(entityfy::detail::primary_key_value(parent, parent_model));
        }

        std::unordered_map<std::string, std::vector<Child>> grouped;

        if(!parent_keys.empty())
        {
            std::string sql = "SELECT ";

            for(std::size_t index = 0; index < child_model.columns.size(); ++index)
            {
                if(index != 0)
                {
                    sql += ", ";
                }

                sql += entityfy::detail::quote_identifier(child_model.columns[index].name);
            }

            sql += " FROM " + entityfy::detail::quote_identifier(child_model.table_name) + " WHERE " +
                   entityfy::detail::quote_identifier(relation.local_column) + " IN (";

            for(std::size_t index = 0; index < parent_keys.size(); ++index)
            {
                if(index != 0)
                {
                    sql += ", ";
                }

                sql += "?";
            }

            sql += ")";

            owner_->log(sql);

            const auto rows = owner_->database().query(
                entityfy::statement{.sql = std::move(sql), .binds = std::move(parent_keys)}
            );

            for(const auto& row: rows)
            {
                auto child = entityfy::detail::materialize_row<Child>(row);

                if(const auto key = entityfy::detail::read_column(child, relation.local_column))
                {
                    grouped[entityfy::detail::canonical_key(*key)].push_back(std::move(child));
                }
            }
        }

        constexpr auto value_navigation = detail::find_member_of_type(^^T, ^^std::vector<Child>);

        for(const auto& parent: parent_rows)
        {
            const auto key = entityfy::detail::primary_key_value(parent, parent_model);
            auto       bucket = grouped[entityfy::detail::canonical_key(key)];

            output.emplace_back(parent, std::move(bucket));

            if constexpr(value_navigation != std::meta::info{})
            {
                output.back().first.[:value_navigation:] = output.back().second;
            }
        }

        return output;
    }

    template <typename Child>
    [[nodiscard]] db_untracked_include_builder<T, Child> include() const;

private:
    db_context*               owner_;
    entityfy::query_options<T> options_;
};

// Builder returned by db_untracked_query_builder::include(): to_list() yields
// owning value pairs (parent, children).
template <typename T, typename Child>
class db_untracked_include_builder
{
public:
    explicit db_untracked_include_builder(db_untracked_query_builder<T> builder)
        : builder_{std::move(builder)}
    {
    }

    [[nodiscard]] std::vector<std::pair<T, std::vector<Child>>> to_list() const
    {
        return builder_.template to_list_with_children<Child>();
    }

private:
    db_untracked_query_builder<T> builder_;
};

template <typename T>
template <typename Child>
db_untracked_include_builder<T, Child> db_untracked_query_builder<T>::include() const
{
    return db_untracked_include_builder<T, Child>{*this};
}

template <typename T>
class db_query_builder
{
public:
    db_query_builder(db_context& owner, std::shared_ptr<detail::set_state<T>> state)
        : owner_{&owner}, state_{std::move(state)}
    {
    }

    [[nodiscard]] db_query_builder where(entityfy::condition filter) const
    {
        auto copy = *this;
        copy.options_.filter = std::move(filter);
        return copy;
    }

    template <typename Member>
    [[nodiscard]] db_query_builder order_by(
        Member T::* member,
        entityfy::sort_direction direction = entityfy::sort_direction::ascending) const
    {
        auto copy = *this;
        copy.options_.orderings.push_back(entityfy::order_by(member, direction));
        return copy;
    }

    [[nodiscard]] db_query_builder take(std::uint64_t count) const
    {
        auto copy = *this;
        copy.options_.limit = count;
        return copy;
    }

    [[nodiscard]] db_query_builder skip(std::uint64_t count) const
    {
        auto copy = *this;
        copy.options_.offset = count;
        return copy;
    }

    [[nodiscard]] std::vector<T*> to_list() const
    {
        const auto model = entityfy::describe_model<T>(owner_->database().target_dialect());
        const auto command = entityfy::select_statement(model, options_);

        owner_->log(command.sql);

        std::vector<T*> output;
        owner_->database().query_each(command, [&](const entityfy::detail::row& row) {
            output.push_back(&state_->attach(entityfy::detail::materialize_row<T>(row)));
            return true;
        });

        return output;
    }

    [[nodiscard]] T* first() const
    {
        auto copy = *this;
        copy.options_.limit = 1;

        const auto rows = copy.to_list();
        return rows.empty() ? nullptr : rows.front();
    }

    template <typename... Members>
    [[nodiscard]] auto select(Members T::*... members) const
    {
        using value_type = std::tuple<std::remove_cvref_t<decltype(std::declval<T>().*members)>...>;

        const auto model = entityfy::describe_model<T>(owner_->database().target_dialect());

        std::string sql = "SELECT ";
        bool first_column = true;

        const auto append = [&](auto member) {
            if(!first_column)
            {
                sql += ", ";
            }

            sql += entityfy::detail::quote_identifier(entityfy::column_name(member));
            first_column = false;
        };

        (append(members), ...);

        sql += " FROM " + entityfy::detail::quote_identifier(model.table_name);

        std::vector<entityfy::sql_value> binds;
        entityfy::detail::append_query_tail(sql, binds, options_);

        owner_->log(sql);

        const auto rows = owner_->database().query(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});

        std::vector<value_type> output;
        output.reserve(rows.size());

        for(const auto& row: rows)
        {
            value_type values{};

            [&]<std::size_t... Index>(std::index_sequence<Index...>) {
                (entityfy::detail::assign_sql_value(row[Index], std::get<Index>(values)), ...);
            }(std::index_sequence_for<Members...>{});

            output.emplace_back(std::move(values));
        }

        return output;
    }

    [[nodiscard]] std::uint64_t count() const
    {
        return owner_->database().template count<T>(options_);
    }

    std::uint64_t remove_all() const
    {
        const auto model = entityfy::describe_model<T>(owner_->database().target_dialect());

        std::string sql = "DELETE FROM " + entityfy::detail::quote_identifier(model.table_name);
        std::vector<entityfy::sql_value> binds;

        if(!options_.filter.empty())
        {
            sql += " WHERE " + options_.filter.sql;
            binds = options_.filter.binds;
        }

        owner_->log(sql);

        const auto result =
            owner_->database().execute(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});
        return static_cast<std::uint64_t>(result.rows_affected);
    }

    template <typename Member>
    [[nodiscard]] auto count_by(Member T::* member) const
    {
        using group_type = std::remove_cvref_t<decltype(std::declval<T>().*member)>;
        return grouped<group_type, std::int64_t>("COUNT(*)", entityfy::column_name(member));
    }

    template <typename... Members>
    [[nodiscard]] auto group_count(Members T::*... members) const
    {
        return group_count_having(entityfy::condition{}, members...);
    }

    template <typename... Members>
    [[nodiscard]] auto group_count_having(entityfy::condition having, Members T::*... members) const
    {
        using key_type = std::tuple<std::remove_cvref_t<decltype(std::declval<T>().*members)>...>;

        const auto model = entityfy::describe_model<T>(owner_->database().target_dialect());

        std::string sql = "SELECT ";

        bool first = true;
        const auto append_column = [&](auto member) {
            if(!first)
            {
                sql += ", ";
            }

            sql += entityfy::detail::quote_identifier(entityfy::column_name(member));
            first = false;
        };

        (append_column(members), ...);

        sql += ", COUNT(*) FROM " + entityfy::detail::quote_identifier(model.table_name);

        std::vector<entityfy::sql_value> binds;

        if(!options_.filter.empty())
        {
            sql += " WHERE " + options_.filter.sql;
            binds = options_.filter.binds;
        }

        sql += " GROUP BY ";
        first = true;
        (append_column(members), ...);

        if(!having.empty())
        {
            sql += " HAVING " + having.sql;
            binds.insert(binds.end(), having.binds.begin(), having.binds.end());
        }

        owner_->log(sql);

        const auto rows = owner_->database().query(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});

        std::map<key_type, std::int64_t> output;

        for(const auto& row: rows)
        {
            key_type key{};

            [&]<std::size_t... Index>(std::index_sequence<Index...>) {
                (entityfy::detail::assign_sql_value(row[Index], std::get<Index>(key)), ...);
            }(std::index_sequence_for<Members...>{});

            std::int64_t count = 0;
            entityfy::detail::assign_sql_value(row[sizeof...(Members)], count);
            output.emplace(std::move(key), count);
        }

        return output;
    }

    template <typename GroupMember, typename ValueMember>
    [[nodiscard]] auto sum_by(GroupMember T::* group, ValueMember T::* value) const
    {
        using group_type = std::remove_cvref_t<decltype(std::declval<T>().*group)>;
        using value_type = std::remove_cvref_t<decltype(std::declval<T>().*value)>;
        return grouped<group_type, value_type>(
            "SUM(" + entityfy::detail::quote_identifier(entityfy::column_name(value)) + ")",
            entityfy::column_name(group));
    }

    template <typename GroupMember, typename ValueMember>
    [[nodiscard]] auto avg_by(GroupMember T::* group, ValueMember T::* value) const
    {
        using group_type = std::remove_cvref_t<decltype(std::declval<T>().*group)>;
        using value_type = std::remove_cvref_t<decltype(std::declval<T>().*value)>;
        const auto dialect = owner_->database().target_dialect();
        const auto column = entityfy::detail::quote_identifier(entityfy::column_name(value));

        return grouped<group_type, double>(
            dialect == entityfy::dialect::sqlserver ? "AVG(CAST(" + column + " AS FLOAT))" : "AVG(" + column + ")",
            entityfy::column_name(group));
    }

    template <typename GroupMember, typename ValueMember>
    [[nodiscard]] auto min_by(GroupMember T::* group, ValueMember T::* value) const
    {
        using group_type = std::remove_cvref_t<decltype(std::declval<T>().*group)>;
        using value_type = std::remove_cvref_t<decltype(std::declval<T>().*value)>;
        return grouped<group_type, value_type>(
            "MIN(" + entityfy::detail::quote_identifier(entityfy::column_name(value)) + ")",
            entityfy::column_name(group));
    }

    template <typename GroupMember, typename ValueMember>
    [[nodiscard]] auto max_by(GroupMember T::* group, ValueMember T::* value) const
    {
        using group_type = std::remove_cvref_t<decltype(std::declval<T>().*group)>;
        using value_type = std::remove_cvref_t<decltype(std::declval<T>().*value)>;
        return grouped<group_type, value_type>(
            "MAX(" + entityfy::detail::quote_identifier(entityfy::column_name(value)) + ")",
            entityfy::column_name(group));
    }

    template <typename Member>
    [[nodiscard]] auto sum(Member T::* member) const
    {
        using member_type = std::remove_cvref_t<decltype(std::declval<T>().*member)>;

        const auto value = aggregate_value("SUM", entityfy::column_name(member));
        if(!value)
        {
            return std::optional<member_type>{};
        }

        member_type output{};
        entityfy::detail::assign_sql_value(*value, output);
        return std::optional<member_type>{output};
    }

    template <typename Member>
    [[nodiscard]] auto min(Member T::* member) const
    {
        using member_type = std::remove_cvref_t<decltype(std::declval<T>().*member)>;

        const auto value = aggregate_value("MIN", entityfy::column_name(member));
        if(!value)
        {
            return std::optional<member_type>{};
        }

        member_type output{};
        entityfy::detail::assign_sql_value(*value, output);
        return std::optional<member_type>{output};
    }

    template <typename Member>
    [[nodiscard]] auto max(Member T::* member) const
    {
        using member_type = std::remove_cvref_t<decltype(std::declval<T>().*member)>;

        const auto value = aggregate_value("MAX", entityfy::column_name(member));
        if(!value)
        {
            return std::optional<member_type>{};
        }

        member_type output{};
        entityfy::detail::assign_sql_value(*value, output);
        return std::optional<member_type>{output};
    }

    template <typename Member>
    [[nodiscard]] std::optional<double> avg(Member T::* member) const
    {
        using member_type = std::remove_cvref_t<decltype(std::declval<T>().*member)>;

        const auto value = aggregate_value("AVG", entityfy::column_name(member));
        if(!value)
        {
            return std::nullopt;
        }

        double output{};
        entityfy::detail::assign_sql_value(*value, output);
        return std::optional<double>{output};
    }

    template <typename Member, typename Value>
    [[nodiscard]] db_query_builder assign(Member T::* member, Value&& value) const
    {
        auto copy = *this;
        copy.assignments_.emplace_back(entityfy::column_name(member),
                                       entityfy::detail::to_sql_value(std::forward<Value>(value)));
        return copy;
    }

    std::uint64_t update_all() const
    {
        if(assignments_.empty())
        {
            throw std::runtime_error{"entityfy: update_all() requires at least one assign()"};
        }

        const auto model = entityfy::describe_model<T>(owner_->database().target_dialect());

        std::string sql = "UPDATE " + entityfy::detail::quote_identifier(model.table_name) + " SET ";

        std::vector<entityfy::sql_value> binds;
        binds.reserve(assignments_.size());

        for(std::size_t index = 0; index < assignments_.size(); ++index)
        {
            if(index != 0)
            {
                sql += ", ";
            }

            sql += entityfy::detail::quote_identifier(assignments_[index].first) + " = ?";
            binds.push_back(assignments_[index].second);
        }

        if(!options_.filter.empty())
        {
            sql += " WHERE " + options_.filter.sql;
            binds.insert(binds.end(), options_.filter.binds.begin(), options_.filter.binds.end());
        }

        owner_->log(sql);

        const auto result =
            owner_->database().execute(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});
        return static_cast<std::uint64_t>(result.rows_affected);
    }

private:
    [[nodiscard]] std::optional<entityfy::sql_value> aggregate_value(
        std::string_view function,
        std::string_view column) const
    {
        const auto model = entityfy::describe_model<T>(owner_->database().target_dialect());

        std::string expression;
        if(function == "AVG" && model.target == entityfy::dialect::sqlserver)
        {
            // T-SQL AVG over an integer column returns an integer; cast first.
            expression = "AVG(CAST(" + entityfy::detail::quote_identifier(column) + " AS FLOAT))";
        }
        else
        {
            expression = std::string{function} + "(" + entityfy::detail::quote_identifier(column) + ")";
        }

        std::string sql = "SELECT " + expression + " FROM " +
                          entityfy::detail::quote_identifier(model.table_name);

        std::vector<entityfy::sql_value> binds;

        if(!options_.filter.empty())
        {
            sql += " WHERE " + options_.filter.sql;
            binds = options_.filter.binds;
        }

        owner_->log(sql);

        const auto rows = owner_->database().query(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});

        if(rows.empty() || rows.front().empty() || entityfy::detail::is_null(rows.front().front()))
        {
            return std::nullopt;
        }

        return rows.front().front();
    }

    template <typename Group, typename Result>
    [[nodiscard]] std::vector<std::pair<Group, Result>> grouped(
        std::string aggregate_expression,
        std::string_view group_column) const
    {
        const auto model = entityfy::describe_model<T>(owner_->database().target_dialect());

        std::string sql = "SELECT " + entityfy::detail::quote_identifier(group_column) + ", " +
                          aggregate_expression + " FROM " +
                          entityfy::detail::quote_identifier(model.table_name);

        std::vector<entityfy::sql_value> binds;

        if(!options_.filter.empty())
        {
            sql += " WHERE " + options_.filter.sql;
            binds = options_.filter.binds;
        }

        sql += " GROUP BY " + entityfy::detail::quote_identifier(group_column) + " ORDER BY " +
               entityfy::detail::quote_identifier(group_column);

        owner_->log(sql);

        const auto rows = owner_->database().query(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});

        std::vector<std::pair<Group, Result>> output;
        output.reserve(rows.size());

        for(const auto& row: rows)
        {
            if(row.size() < 2)
            {
                throw std::runtime_error{"entityfy: grouped query returned fewer than two columns"};
            }

            Group group{};
            Result result{};
            entityfy::detail::assign_sql_value(row[0], group);
            entityfy::detail::assign_sql_value(row[1], result);
            output.emplace_back(std::move(group), std::move(result));
        }

        return output;
    }

private:
    db_context*                             owner_;
    std::shared_ptr<detail::set_state<T>>    state_;
    entityfy::query_options<T>               options_;
    std::vector<std::pair<std::string, entityfy::sql_value>> assignments_;
};

template <typename T>
db_query_builder<T> db_set<T>::query()
{
    return db_query_builder<T>{*owner_, state()};
}

template <typename T>
db_untracked_query_builder<T> db_set<T>::as_no_tracking()
{
    return db_untracked_query_builder<T>{*owner_};
}

template <typename T>
db_set<T> db_context::set()
{
    return db_set<T>{*this};
}

template <typename T>
void db_context::register_model()
{
    const auto key = std::type_index(typeid(T));
    if(model_index_.contains(key))
    {
        return;
    }

    const auto model = entityfy::describe_model<T>(database_.target_dialect());

    model_info info;
    info.table = model.table_name;

    for(const auto& column: model.columns)
    {
        info.columns.push_back(column.name);
    }

    for(const auto& relation: model.relations)
    {
        info.foreign_keys.push_back(relation.referenced_table);
    }

    model_index_.emplace(key, models_.size());
    models_.push_back(std::move(info));
    migrators_.push_back([this] { database_.template table<T>().migrate(); });
}

template <typename T>
std::shared_ptr<detail::set_state<T>> db_context::state_for()
{
    const auto key = std::type_index(typeid(T));
    auto found = states_.find(key);

    if(found == states_.end())
    {
        auto state = std::make_shared<detail::set_state<T>>(*this);
        found = states_.emplace(key, state).first;
        order_.push_back(state);
    }

    return std::static_pointer_cast<detail::set_state<T>>(found->second);
}

} // namespace entityfy
