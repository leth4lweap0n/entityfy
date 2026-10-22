#pragma once

#include <entityfy/db_context.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
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

consteval std::meta::info find_primary_key(std::meta::info type)
{
    for(const auto member: std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked()))
    {
        if(!std::meta::has_identifier(member))
        {
            continue;
        }

        bool candidate = std::meta::identifier_of(member) == "id";

        for(const auto annotation: std::meta::annotations_of(member))
        {
            if(std::meta::type_of(annotation) == ^^entityfy::primary_key_t)
            {
                candidate = true;
            }
        }

        if(candidate)
        {
            return member;
        }
    }

    throw "entityfy: model has no primary key";
}

consteval std::string_view member_column_name(std::meta::info member)
{
    return std::meta::identifier_of(member);
}

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

consteval bool is_ignored_member(std::meta::info member)
{
    return !std::meta::annotations_of_with_type(member, ^^entityfy::ignore_t).empty();
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

template <typename T>
std::int64_t key_of(const T& entity)
{
    constexpr auto member = find_primary_key(^^T);
    using member_type = std::remove_cvref_t<decltype(entity.[:member:])>;

    static_assert(std::is_integral_v<member_type>,
                  "entityfy: Phase 1 requires an integral primary key for change tracking");

    return static_cast<std::int64_t>(entity.[:member:]);
}

template <typename T>
void assign_generated_id(T& entity, std::int64_t value)
{
    constexpr auto member = find_primary_key(^^T);
    using member_type = std::remove_cvref_t<decltype(entity.[:member:])>;

    if constexpr(std::is_integral_v<member_type>)
    {
        entity.[:member:] = static_cast<member_type>(value);
    }
}

template <typename T>
bool is_persisted_column(const entityfy::model_descriptor<T>& model, std::string_view name)
{
    return std::ranges::any_of(model.columns, [&](const entityfy::column_descriptor& column) {
        return column.name == name;
    });
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
                result = entityfy::to_sql_value(entity.[:meta_field:]);
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
    };

    std::deque<std::unique_ptr<tracked>>      items;
    std::unordered_map<std::int64_t, std::size_t> identity;
    std::deque<tracked>                        backup;
    std::string                                table_name_;
    std::vector<std::string>                   referenced_tables_;

    explicit set_state(entityfy::dialect target)
    {
        const auto model = entityfy::describe_model<T>(target);
        table_name_ = model.table_name;

        for(const auto& relation: model.relations)
        {
            referenced_tables_.push_back(relation.referenced_table);
        }
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
        backup.clear();
        for(const auto& holder: items)
        {
            backup.push_back(*holder);
        }
    }

    void commit_save() noexcept override
    {
        backup.clear();
    }

    void rollback_save() override
    {
        constexpr auto version_member = find_member(^^T, "version");
        constexpr bool has_version_member = version_member != std::meta::info{};

        if(items.size() != backup.size())
        {
            items.clear();
            for(const auto& snapshot: backup)
            {
                items.push_back(std::make_unique<tracked>(snapshot));
            }
        }
        else
        {
            for(std::size_t index = 0; index < items.size(); ++index)
            {
                items[index]->original = backup[index].original;
                items[index]->state = backup[index].state;

                if(backup[index].state == entity_state::added)
                {
                    items[index]->entity = backup[index].entity;
                }
                else if constexpr(has_version_member)
                {
                    items[index]->entity.[:version_member:] = backup[index].original.[:version_member:];
                }
            }
        }

        backup.clear();
        rebuild_identity();
    }

    save_result delete_removed(db_context& owner) override;
    save_result save(db_context& owner) override;

    T& add(T value)
    {
        const std::int64_t key = key_of(value);
        if(key != 0 && identity.contains(key))
        {
            throw std::runtime_error{"entityfy: an entity with the same key is already tracked"};
        }

        items.push_back(std::make_unique<tracked>(tracked{.entity = std::move(value)}));
        auto& item = *items.back();
        item.original = item.entity;
        item.state = entity_state::added;

        if(key != 0)
        {
            identity[key] = items.size() - 1;
        }

        return item.entity;
    }

    T& attach(T value)
    {
        const std::int64_t key = key_of(value);

        if(key != 0)
        {
            if(const auto found = identity.find(key); found != identity.end())
            {
                return items[found->second]->entity;
            }
        }

        items.push_back(std::make_unique<tracked>(tracked{.entity = std::move(value)}));
        auto& item = *items.back();
        item.original = item.entity;
        item.state = entity_state::unchanged;

        if(key != 0)
        {
            identity[key] = items.size() - 1;
        }

        return item.entity;
    }

    void remove(const T& value)
    {
        const std::int64_t key = key_of(value);
        if(const auto found = identity.find(key); found != identity.end())
        {
            items[found->second]->state = entity_state::removed;
            return;
        }

        for(auto& holder: items)
        {
            auto& item = *holder;
            if(&item.entity == &value)
            {
                item.state = entity_state::removed;
                return;
            }
        }

        throw std::runtime_error{"entityfy: remove() called for an untracked entity"};
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

        const auto model = entityfy::describe_model<T>(entityfy::dialect::sqlite);
        constexpr auto version_member = find_member(^^T, "version");
        constexpr bool has_version_member = version_member != std::meta::info{};

        bool modified = false;

        template for (constexpr auto member: std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())))
        {
            if constexpr(!is_ignored_member(member))
            {
                if constexpr(member == find_primary_key(^^T))
                {
                    continue;
                }

                if constexpr(has_version_member)
                {
                    if constexpr(member == version_member)
                    {
                        continue;
                    }
                }

                const std::string column{member_column_name(member)};
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
    }

    void refresh(db_context& owner, T& value)
    {
        const auto index = index_of(value);
        if(!index)
        {
            throw std::runtime_error{"entityfy: refresh() called for an untracked entity"};
        }

        const std::int64_t key = key_of(value);
        if(key == 0)
        {
            throw std::runtime_error{"entityfy: cannot refresh an unsaved entity"};
        }

        auto fetched = owner.Database().template find<T>(key);
        if(!fetched)
        {
            throw std::runtime_error{"entityfy: refresh() could not find the entity"};
        }

        value = *fetched;

        auto& item = *items[*index];
        item.entity = value;
        item.original = value;
        item.state = entity_state::unchanged;
        identity[key] = *index;
    }

    void rebuild_identity()
    {
        identity.clear();
        for(std::size_t index = 0; index < items.size(); ++index)
        {
            const std::int64_t key = key_of(items[index]->entity);
            if(key != 0)
            {
                identity[key] = index;
            }
        }
    }

    [[nodiscard]] std::optional<std::size_t> index_of(const T& value) const
    {
        const std::int64_t key = key_of(value);
        if(key != 0)
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
    const auto model = entityfy::describe_model<T>(owner.Database().target_dialect());

    for(auto& holder: items)
    {
        auto& item = *holder;
        if(item.state != entity_state::removed)
        {
            continue;
        }

        const std::int64_t key = key_of(item.entity);
        if(key != 0)
        {
            auto command = entityfy::delete_by_id_statement(model, key);
            owner.Log(command.sql);
            result.removed += static_cast<std::size_t>(owner.Database().execute(command).rows_affected);
        }

        item.state = entity_state::detached;
    }

    return result;
}

template <typename T>
save_result set_state<T>::save(db_context& owner)
{
    save_result result;

    const auto model = entityfy::describe_model<T>(owner.Database().target_dialect());
    const std::string table = entityfy::detail::quote_identifier(model.table_name);
    constexpr auto primary_key = find_primary_key(^^T);
    const std::string primary_key_column{member_column_name(find_primary_key(^^T))};

    constexpr auto version_member = find_member(^^T, "version");
    constexpr bool has_version_member = version_member != std::meta::info{};
    const bool has_version = has_version_member && is_persisted_column(model, "version");

    bool generated_primary_key = false;
    for(const auto& column: model.columns)
    {
        if(column.flags.primary_key && column.flags.auto_increment)
        {
            generated_primary_key = true;
        }
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
        const std::string column{member_column_name(member)};
        if(!is_persisted_column(model, column) || generated_column(column))
        {
            continue;
        }

        insert_columns.push_back(column);
    }

    std::vector<std::size_t> added_indices;
    for(std::size_t index = 0; index < items.size(); ++index)
    {
        if(items[index]->state == entity_state::added)
        {
            added_indices.push_back(index);
        }
    }

    for(const std::size_t index: added_indices)
    {
        if(generated_primary_key && key_of(items[index]->entity) != 0)
        {
            throw std::runtime_error{"entityfy: explicit primary key is not allowed for an auto-increment model"};
        }
    }

    for(std::size_t index = 0; index < items.size(); ++index)
    {
        auto& item = *items[index];
        if(item.state == entity_state::removed)
        {
            item.state = entity_state::detached;
        }
    }

    if(!added_indices.empty() && !insert_columns.empty() && !generated_primary_key)
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
            std::max<std::size_t>(1, owner.BatchParameterLimit() / insert_columns.size());

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
                        const std::string column{member_column_name(member)};

                        if(std::ranges::find(insert_columns, column) == insert_columns.end())
                        {
                            continue;
                        }

                        if(!first_column)
                        {
                            sql += ", ";
                        }

                        sql += "?";
                        binds.push_back(entityfy::to_sql_value(entity.[:member:]));
                        first_column = false;
                    }
                }

                sql += ")";
            }

            owner.Log(sql);

            const auto executed =
                owner.Database().execute(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});

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
                item.original = item.entity;
                item.state = entity_state::unchanged;

                if(const std::int64_t key = key_of(item.entity); key != 0)
                {
                    identity[key] = index;
                }
            }
        }
    }

    for(std::size_t index = 0; index < items.size(); ++index)
    {
        auto& item = *items[index];

        if(item.state == entity_state::detached)
        {
            continue;
        }

        if(item.state == entity_state::added)
        {
            if(generated_primary_key && key_of(item.entity) != 0)
            {
                throw std::runtime_error{"entityfy: explicit primary key is not allowed for an auto-increment model"};
            }

            auto command = entityfy::insert_statement(item.entity, model);
            owner.Log(command.sql);

            const auto inserted = owner.Database().execute(command);
            result.inserted += static_cast<std::size_t>(inserted.rows_affected);

            if(generated_primary_key && key_of(item.entity) == 0)
            {
                std::int64_t generated = inserted.last_insert_id;

                if(generated == 0 && owner.Database().target_dialect() == entityfy::dialect::postgresql)
                {
                    const auto rows = owner.Database().query(entityfy::statement{.sql = "SELECT lastval()"});
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

            item.original = item.entity;
            item.state = entity_state::unchanged;

            if(const std::int64_t key = key_of(item.entity); key != 0)
            {
                identity[key] = index;
            }

            continue;
        }

        if(item.state == entity_state::removed)
        {
            const std::int64_t key = key_of(item.entity);
            if(key != 0)
            {
                auto command = entityfy::delete_by_id_statement(model, key);
                owner.Log(command.sql);
                result.removed += static_cast<std::size_t>(owner.Database().execute(command).rows_affected);
            }

            continue;
        }

        std::string sql = "UPDATE " + table + " SET ";
        std::vector<entityfy::sql_value> binds;
        bool first = true;

        template for (constexpr auto member: std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())))
        {
            if constexpr(!is_ignored_member(member))
            {
                if constexpr(member == find_primary_key(^^T))
                {
                    continue;
                }

                if constexpr(has_version_member)
                {
                    if constexpr(member == version_member)
                    {
                        continue;
                    }
                }

                const std::string column{member_column_name(member)};
                if(!is_persisted_column(model, column))
                {
                    continue;
                }

                const auto& current = item.entity.[:member:];
                const auto& original = item.original.[:member:];

                if(current == original)
                {
                    continue;
                }

                if(!first)
                {
                    sql += ", ";
                }

                sql += entityfy::detail::quote_identifier(column) + " = ?";
                binds.push_back(entityfy::to_sql_value(current));
                first = false;
            }
        }

        if(first)
        {
            item.original = item.entity;
            continue;
        }

        if constexpr(has_version_member)
        {
            if(has_version)
            {
                sql += ", " + entityfy::detail::quote_identifier("version") + " = " +
                       entityfy::detail::quote_identifier("version") + " + 1";
            }
        }

        sql += " WHERE " + entityfy::detail::quote_identifier(primary_key_column) + " = ?";
        binds.push_back(entityfy::to_sql_value(item.entity.[:primary_key:]));

        if constexpr(has_version_member)
        {
            if(has_version)
            {
                sql += " AND " + entityfy::detail::quote_identifier("version") + " = ?";
                binds.push_back(entityfy::to_sql_value(item.original.[:version_member:]));
            }
        }

        owner.Log(sql);

        const auto updated = owner.Database().execute(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});
        result.updated += static_cast<std::size_t>(updated.rows_affected);

        if(updated.rows_affected == 0)
        {
            throw std::runtime_error{has_version
                                         ? "entityfy: optimistic concurrency conflict for key " +
                                               std::to_string(key_of(item.entity))
                                         : "entityfy: update affected zero rows for key " +
                                               std::to_string(key_of(item.entity))};
        }

        if constexpr(has_version_member)
        {
            if(has_version)
            {
                using version_type = std::remove_cvref_t<decltype(item.entity.[:version_member:])>;
                static_assert(std::is_integral_v<version_type>, "entityfy: version column must be an integral type");

                item.entity.[:version_member:] =
                    static_cast<version_type>(item.entity.[:version_member:] + static_cast<version_type>(1));
            }
        }

        item.original = item.entity;
    }

    const bool has_removed = std::ranges::any_of(items, [](const std::unique_ptr<tracked>& holder) {
        return holder->state == entity_state::removed || holder->state == entity_state::detached;
    });

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
    }

    rebuild_identity();

    return result;
}

} // namespace detail

template <typename T>
class db_query_builder;

// The DbSet equivalent: a typed handle over one entity type. It is a lazy
// view, so it can be declared as a member of a user defined context:
//
//     struct app_context : entityfy::db_context
//     {
//         using entityfy::db_context::db_context;
//         entityfy::db_set<user> Users{*this};
//     };
//
//     ctx.Users.Add(user{...});
//     ctx.SaveChanges();
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
    T& Add(T value)
    {
        return state()->add(std::move(value));
    }

    // Tracks several new entities at once.
    void AddRange(const std::vector<T>& values)
    {
        for(const auto& value: values)
        {
            Add(value);
        }
    }

    // Tracks an entity without marking it as new.
    T& Attach(T value)
    {
        return state()->attach(std::move(value));
    }

    // Marks a tracked entity as removed.
    void Remove(const T& value)
    {
        state()->remove(value);
    }

    [[nodiscard]] entity_state State(const T& value) const
    {
        return state()->state_of(value);
    }

    void Detach(const T& value)
    {
        state()->detach(value);
    }

    void Refresh(T& value)
    {
        state()->refresh(*owner_, value);
    }

    // Loads an entity by primary key. Returns nullptr when it does not exist.
    [[nodiscard]] T* Find(std::int64_t id)
    {
        auto tracked_state = state();

        if(const auto found = tracked_state->identity.find(id); found != tracked_state->identity.end())
        {
            return &tracked_state->items[found->second]->entity;
        }

        const auto model = entityfy::describe_model<T>(owner_->Database().target_dialect());
        auto command = entityfy::select_by_id_statement(model, id);
        owner_->Log(command.sql);

        const auto rows = owner_->Database().query(command);
        if(rows.empty())
        {
            return nullptr;
        }

        return &tracked_state->attach(entityfy::detail::materialize_row<T>(rows.front()));
    }

    // Loads every row of the table and tracks it.
    [[nodiscard]] std::vector<T*> ToList(std::size_t limit = 0)
    {
        auto tracked_state = state();

        entityfy::query_options<T> options;
        if(limit != 0)
        {
            options.limit = limit;
        }

        const auto model = entityfy::describe_model<T>(owner_->Database().target_dialect());
        auto command = entityfy::select_statement(model, options);
        owner_->Log(command.sql);

        const auto rows = owner_->Database().query(command);

        std::vector<T*> output;
        output.reserve(rows.size());

        for(const auto& row: rows)
        {
            output.push_back(&tracked_state->attach(entityfy::detail::materialize_row<T>(row)));
        }

        return output;
    }

    // Number of entities currently tracked by this set (EF: DbSet.Local.Count).
    // The database count is Query().Count().
    [[nodiscard]] std::size_t LocalCount() const noexcept
    {
        return state()->items.size();
    }

    [[nodiscard]] db_query_builder<T> Query();

    // Loads the rows produced by a raw SQL statement and tracks them.
    [[nodiscard]] std::vector<T*> FromSql(std::string_view sql)
    {
        auto tracked_state = state();
        owner_->Log(sql);

        const auto rows = owner_->Database().query(entityfy::statement{.sql = std::string{sql}});

        std::vector<T*> output;
        output.reserve(rows.size());

        for(const auto& row: rows)
        {
            output.push_back(&tracked_state->attach(entityfy::detail::materialize_row<T>(row)));
        }

        return output;
    }

    template <typename Other, typename Member, typename OtherMember>
    [[nodiscard]] std::vector<std::pair<T*, Other*>> Join(Member T::* left, OtherMember Other::* right)
    {
        const auto dialect = owner_->Database().target_dialect();
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

        owner_->Log(sql);

        const auto rows = owner_->Database().query(entityfy::statement{.sql = std::move(sql)});
        auto other_set = owner_->template Set<Other>();

        const auto split = left_model.columns.size();

        std::vector<std::pair<T*, Other*>> output;
        output.reserve(rows.size());

        for(const auto& row: rows)
        {
            if(row.size() < split)
            {
                throw std::runtime_error{"entityfy: join row has fewer columns than the left model"};
            }

            std::vector<entityfy::sql_value> left_row(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(split));
            std::vector<entityfy::sql_value> right_row(row.begin() + static_cast<std::ptrdiff_t>(split), row.end());

            auto left_value = entityfy::detail::materialize_row<T>(left_row);
            auto right_value = entityfy::detail::materialize_row<Other>(right_row);

            output.emplace_back(&state()->attach(std::move(left_value)),
                                &other_set.Attach(std::move(right_value)));
        }

        return output;
    }

    template <typename Other, typename Third, typename Member, typename OtherMember,
              typename ThirdMember, typename Member2>
    [[nodiscard]] std::vector<std::tuple<T*, Other*, Third*>> Join(
        Member T::* left,
        OtherMember Other::* right,
        ThirdMember Third::* third,
        Member2 T::* left2)
    {
        const auto dialect = owner_->Database().target_dialect();
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

        owner_->Log(sql);

        const auto rows = owner_->Database().query(entityfy::statement{.sql = std::move(sql)});
        auto middle_set = owner_->template Set<Other>();
        auto right_set = owner_->template Set<Third>();

        const auto first_split = left_model.columns.size();
        const auto second_split = first_split + middle_model.columns.size();

        std::vector<std::tuple<T*, Other*, Third*>> output;
        output.reserve(rows.size());

        for(const auto& row: rows)
        {
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
                                &middle_set.Attach(std::move(middle_value)),
                                &right_set.Attach(std::move(right_value)));
        }

        return output;
    }

    // Eager loading of the children of the given parents. The foreign key is
    // resolved from the "references" annotation, so the caller does not pass a
    // member pointer.
    template <typename Child>
    [[nodiscard]] std::vector<std::pair<T*, std::vector<Child*>>> Include(const std::vector<T*>& parents)
    {
        std::vector<std::pair<T*, std::vector<Child*>>> output;
        output.reserve(parents.size());

        if(parents.empty())
        {
            return output;
        }

        const auto dialect = owner_->Database().target_dialect();
        const auto parent_model = entityfy::describe_model<T>(dialect);
        const auto child_model = entityfy::describe_model<Child>(dialect);
        const auto& relation = entityfy::detail::find_relation<Child, T>(child_model, parent_model);

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

        owner_->Log(sql);

        const auto rows = owner_->Database().query(
            entityfy::statement{.sql = std::move(sql), .binds = std::move(parent_keys)}
        );

        std::unordered_map<std::string, std::vector<Child*>> grouped;

        for(const auto& row: rows)
        {
            auto child = entityfy::detail::materialize_row<Child>(row);

            const auto key = entityfy::detail::read_column(child, relation.local_column);
            if(!key)
            {
                continue;
            }

            auto child_set = owner_->template Set<Child>();
            auto& stored = child_set.Attach(std::move(child));
            grouped[entityfy::detail::canonical_key(*key)].push_back(&stored);
        }

        for(T* parent: parents)
        {
            const auto key = entityfy::detail::primary_key_value(*parent, parent_model);
            auto bucket = grouped[entityfy::detail::canonical_key(key)];

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
    [[nodiscard]] std::vector<std::pair<T*, Parent*>> IncludeParent(const std::vector<T*>& children)
    {
        std::vector<std::pair<T*, Parent*>> output;
        output.reserve(children.size());

        if(children.empty())
        {
            return output;
        }

        const auto dialect = owner_->Database().target_dialect();
        const auto child_model = entityfy::describe_model<T>(dialect);
        const auto parent_model = entityfy::describe_model<Parent>(dialect);
        const auto& relation = entityfy::detail::find_relation<T, Parent>(child_model, parent_model);

        const auto parent_key_column = entityfy::primary_key_column(parent_model).name;
        if(relation.referenced_column != parent_key_column)
        {
            throw std::runtime_error{"entityfy: relation from " + child_model.table_name + " to " +
                                     parent_model.table_name + " does not point at the primary key"};
        }

        std::vector<std::string> keys;
        std::vector<entityfy::sql_value> binds;

        for(auto* child: children)
        {
            const auto key = entityfy::detail::read_column(*child, relation.local_column);
            if(!key)
            {
                continue;
            }

            const auto canonical = entityfy::detail::canonical_key(*key);
            if(std::ranges::find(keys, canonical) == keys.end())
            {
                keys.push_back(canonical);
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

        owner_->Log(sql);

        const auto rows = owner_->Database().query(
            entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)}
        );

        std::unordered_map<std::string, Parent*> loaded;
        auto parent_set = owner_->template Set<Parent>();

        for(const auto& row: rows)
        {
            auto value = entityfy::detail::materialize_row<Parent>(row);
            const auto key = entityfy::detail::primary_key_value(value, parent_model);
            Parent& stored = parent_set.Attach(std::move(value));
            loaded[entityfy::detail::canonical_key(key)] = &stored;
        }

        for(auto* child: children)
        {
            const auto key = entityfy::detail::read_column(*child, relation.local_column);
            if(!key)
            {
                output.emplace_back(child, nullptr);
                continue;
            }

            const auto found = loaded.find(entityfy::detail::canonical_key(*key));
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

template <typename T>
class db_query_builder
{
public:
    db_query_builder(db_context& owner, std::shared_ptr<detail::set_state<T>> state)
        : owner_{&owner}, state_{std::move(state)}
    {
    }

    [[nodiscard]] db_query_builder Where(entityfy::condition filter) const
    {
        auto copy = *this;
        copy.options_.filter = std::move(filter);
        return copy;
    }

    template <typename Member>
    [[nodiscard]] db_query_builder OrderBy(
        Member T::* member,
        entityfy::sort_direction direction = entityfy::sort_direction::ascending) const
    {
        auto copy = *this;
        copy.options_.orderings.push_back(entityfy::order_by(member, direction));
        return copy;
    }

    [[nodiscard]] db_query_builder Take(std::uint64_t count) const
    {
        auto copy = *this;
        copy.options_.limit = count;
        return copy;
    }

    [[nodiscard]] db_query_builder Skip(std::uint64_t count) const
    {
        auto copy = *this;
        copy.options_.offset = count;
        return copy;
    }

    [[nodiscard]] std::vector<T*> ToList() const
    {
        const auto model = entityfy::describe_model<T>(owner_->Database().target_dialect());
        const auto command = entityfy::select_statement(model, options_);

        owner_->Log(command.sql);

        const auto rows = owner_->Database().query(command);

        std::vector<T*> output;
        output.reserve(rows.size());

        for(const auto& row: rows)
        {
            output.push_back(&state_->attach(entityfy::detail::materialize_row<T>(row)));
        }

        return output;
    }

    [[nodiscard]] T* First() const
    {
        auto copy = *this;
        copy.options_.limit = 1;

        const auto rows = copy.ToList();
        return rows.empty() ? nullptr : rows.front();
    }

    template <typename... Members>
    [[nodiscard]] auto Select(Members T::*... members) const
    {
        using value_type = std::tuple<std::remove_cvref_t<decltype(std::declval<T>().*members)>...>;

        const auto model = entityfy::describe_model<T>(owner_->Database().target_dialect());

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

        owner_->Log(sql);

        const auto rows = owner_->Database().query(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});

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

    [[nodiscard]] std::uint64_t Count() const
    {
        return owner_->Database().template count<T>(options_);
    }

    std::uint64_t RemoveAll() const
    {
        const auto model = entityfy::describe_model<T>(owner_->Database().target_dialect());

        std::string sql = "DELETE FROM " + entityfy::detail::quote_identifier(model.table_name);
        std::vector<entityfy::sql_value> binds;

        if(!options_.filter.empty())
        {
            sql += " WHERE " + options_.filter.sql;
            binds = options_.filter.binds;
        }

        owner_->Log(sql);

        const auto result =
            owner_->Database().execute(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});
        return static_cast<std::uint64_t>(result.rows_affected);
    }

    template <typename Member>
    [[nodiscard]] auto CountBy(Member T::* member) const
    {
        using group_type = std::remove_cvref_t<decltype(std::declval<T>().*member)>;
        return grouped<group_type, std::int64_t>("COUNT(*)", entityfy::column_name(member));
    }

    template <typename... Members>
    [[nodiscard]] auto GroupCount(Members T::*... members) const
    {
        return GroupCountHaving(entityfy::condition{}, members...);
    }

    template <typename... Members>
    [[nodiscard]] auto GroupCountHaving(entityfy::condition having, Members T::*... members) const
    {
        using key_type = std::tuple<std::remove_cvref_t<decltype(std::declval<T>().*members)>...>;

        const auto model = entityfy::describe_model<T>(owner_->Database().target_dialect());

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

        owner_->Log(sql);

        const auto rows = owner_->Database().query(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});

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
    [[nodiscard]] auto SumBy(GroupMember T::* group, ValueMember T::* value) const
    {
        using group_type = std::remove_cvref_t<decltype(std::declval<T>().*group)>;
        using value_type = std::remove_cvref_t<decltype(std::declval<T>().*value)>;
        return grouped<group_type, value_type>(
            "SUM(" + entityfy::detail::quote_identifier(entityfy::column_name(value)) + ")",
            entityfy::column_name(group));
    }

    template <typename GroupMember, typename ValueMember>
    [[nodiscard]] auto AvgBy(GroupMember T::* group, ValueMember T::* value) const
    {
        using group_type = std::remove_cvref_t<decltype(std::declval<T>().*group)>;
        using value_type = std::remove_cvref_t<decltype(std::declval<T>().*value)>;
        return grouped<group_type, double>(
            "AVG(" + entityfy::detail::quote_identifier(entityfy::column_name(value)) + ")",
            entityfy::column_name(group));
    }

    template <typename GroupMember, typename ValueMember>
    [[nodiscard]] auto MinBy(GroupMember T::* group, ValueMember T::* value) const
    {
        using group_type = std::remove_cvref_t<decltype(std::declval<T>().*group)>;
        using value_type = std::remove_cvref_t<decltype(std::declval<T>().*value)>;
        return grouped<group_type, value_type>(
            "MIN(" + entityfy::detail::quote_identifier(entityfy::column_name(value)) + ")",
            entityfy::column_name(group));
    }

    template <typename GroupMember, typename ValueMember>
    [[nodiscard]] auto MaxBy(GroupMember T::* group, ValueMember T::* value) const
    {
        using group_type = std::remove_cvref_t<decltype(std::declval<T>().*group)>;
        using value_type = std::remove_cvref_t<decltype(std::declval<T>().*value)>;
        return grouped<group_type, value_type>(
            "MAX(" + entityfy::detail::quote_identifier(entityfy::column_name(value)) + ")",
            entityfy::column_name(group));
    }

    template <typename Member>
    [[nodiscard]] auto Sum(Member T::* member) const
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
    [[nodiscard]] auto Min(Member T::* member) const
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
    [[nodiscard]] auto Max(Member T::* member) const
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
    [[nodiscard]] std::optional<double> Avg(Member T::* member) const
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
    [[nodiscard]] db_query_builder Assign(Member T::* member, Value&& value) const
    {
        auto copy = *this;
        copy.assignments_.emplace_back(entityfy::column_name(member),
                                       entityfy::detail::to_sql_value(std::forward<Value>(value)));
        return copy;
    }

    std::uint64_t UpdateAll() const
    {
        if(assignments_.empty())
        {
            throw std::runtime_error{"entityfy: UpdateAll() requires at least one Assign()"};
        }

        const auto model = entityfy::describe_model<T>(owner_->Database().target_dialect());

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

        owner_->Log(sql);

        const auto result =
            owner_->Database().execute(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});
        return static_cast<std::uint64_t>(result.rows_affected);
    }

private:
    [[nodiscard]] std::optional<entityfy::sql_value> aggregate_value(
        std::string_view function,
        std::string_view column) const
    {
        const auto model = entityfy::describe_model<T>(owner_->Database().target_dialect());

        std::string sql = "SELECT " + std::string{function} + "(" +
                          entityfy::detail::quote_identifier(column) + ") FROM " +
                          entityfy::detail::quote_identifier(model.table_name);

        std::vector<entityfy::sql_value> binds;

        if(!options_.filter.empty())
        {
            sql += " WHERE " + options_.filter.sql;
            binds = options_.filter.binds;
        }

        owner_->Log(sql);

        const auto rows = owner_->Database().query(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});

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
        const auto model = entityfy::describe_model<T>(owner_->Database().target_dialect());

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

        owner_->Log(sql);

        const auto rows = owner_->Database().query(entityfy::statement{.sql = std::move(sql), .binds = std::move(binds)});

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
db_query_builder<T> db_set<T>::Query()
{
    return db_query_builder<T>{*owner_, state()};
}

template <typename T>
db_set<T> db_context::Set()
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
        auto state = std::make_shared<detail::set_state<T>>(database_.target_dialect());
        found = states_.emplace(key, state).first;
        order_.push_back(state);
    }

    return std::static_pointer_cast<detail::set_state<T>>(found->second);
}

} // namespace entityfy
