#pragma once

#include <entityfy/detail/backend.hpp>
#include <entityfy/error.hpp>
#include <entityfy/introspection.hpp>
#include <entityfy/query.hpp>
#include <entityfy/schema_validation.hpp>
#include <entityfy/statement.hpp>
#include <entityfy/table.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace entityfy
{

// What the backend behind a client can do asynchronously. Callers branch on
// this before using the async entry points: `unsupported` means those calls
// reject instead of quietly running synchronously.
using async_capability = detail::async_capability;

class client
{
public:
    explicit client(std::string_view uri);
    ~client();

    client(client&&) noexcept;
    client& operator=(client&&) noexcept;

    client(const client&) = delete;
    client& operator=(const client&) = delete;

    [[nodiscard]] dialect target_dialect() const noexcept
    {
        return backend_->target_dialect();
    }

    [[nodiscard]] async_capability async_mode() const noexcept
    {
        return backend_->async_mode();
    }

    [[nodiscard]] table_info inspect_table(std::string_view table)
    {
        return backend_->inspect_table(table);
    }

    template <typename Model>
    [[nodiscard]] table_info inspect()
    {
        return backend_->inspect_table(table_name<Model>());
    }

    template <typename Function>
    decltype(auto) transaction(Function&& function)
    {
        const auto level = transaction_depth_;
        const auto savepoint = "entityfy_tx_" + std::to_string(level);
        const bool nested = level != 0;

        begin_transaction(nested, savepoint);
        ++transaction_depth_;
        bool active = true;
        bool depth_released = false;

        const auto release_depth = [&]() noexcept {
            if(!depth_released)
            {
                --transaction_depth_;
                depth_released = true;
            }
        };

        try
        {
            using result_type = std::conditional_t<
                std::is_invocable_v<Function&, client&>,
                std::invoke_result<Function&, client&>,
                std::invoke_result<Function&>
            >::type;

            if constexpr(std::is_void_v<result_type>)
            {
                if constexpr(std::is_invocable_v<Function&, client&>)
                {
                    std::invoke(function, *this);
                }
                else
                {
                    std::invoke(function);
                }

                release_depth();
                commit_transaction(nested, savepoint);
                active = false;
            }
            else
            {
                result_type result = [&]() -> result_type {
                    if constexpr(std::is_invocable_v<Function&, client&>)
                    {
                        return std::invoke(function, *this);
                    }
                    else
                    {
                        return std::invoke(function);
                    }
                }();

                release_depth();
                commit_transaction(nested, savepoint);
                active = false;
                return result;
            }
        }
        catch(...)
        {
            if(active)
            {
                release_depth();
                rollback_transaction(nested, savepoint);
            }

            throw;
        }
    }

    template <typename Model>
    [[nodiscard]] table_client<Model> table()
    {
        return table_client<Model>{*backend_};
    }

    template <typename Model>
    void migrate()
    {
        this->template table<Model>().migrate();
    }

    template <typename Model>
    void migrate(schema_sync_options options)
    {
        this->template table<Model>().migrate(std::move(options));
    }

    template <typename Model>
    void migrate_force(schema_validation_options validation = {})
    {
        this->template table<Model>().migrate_force(std::move(validation));
    }

    template <typename Model>
    void reset_schema()
    {
        this->template table<Model>().reset_schema();
    }

    // TPH: creates the shared table from the base and every derived model, so
    // the table carries the union of their columns. Derived models cannot be
    // migrated on their own (their table belongs to the hierarchy).
    template <typename Base, typename... Types>
    void migrate_tph()
    {
        auto model = describe_model<Base>(backend_->target_dialect());

        const auto base_columns = model.columns;
        const auto in_base = [&](std::string_view name) {
            return std::ranges::any_of(base_columns, [&](const column_descriptor& column) {
                return column.name == name;
            });
        };

        const auto merge = [&](const auto& other) {
            if(other.table_name != model.table_name)
            {
                throw std::logic_error{"entityfy::migrate_tph requires every type to share the base table"};
            }

            for(const auto& column : other.columns)
            {
                const auto existing = std::ranges::find_if(model.columns, [&](const column_descriptor& candidate) {
                    return candidate.name == column.name;
                });

                if(existing == model.columns.end())
                {
                    model.columns.push_back(column);
                }
                else if(existing->sql_type != column.sql_type)
                {
                    throw std::logic_error{
                        "entityfy::migrate_tph found conflicting column definitions for `" + column.name + "`"
                    };
                }
            }
        };

        (merge(describe_model<Types>(backend_->target_dialect())), ...);

        // A row only carries the columns of its own type, so the columns that
        // belong to a single subtype have to be nullable in the shared table.
        for(auto& column : model.columns)
        {
            if(!in_base(column.name) && !column.flags.primary_key)
            {
                column.flags.not_null = false;
                column.flags.nullable = true;
            }
        }

        backend_->execute(create_table(model));

        std::vector<std::string> existing_indexes;
        if(model.target == dialect::mysql || model.target == dialect::sqlserver)
        {
            // Neither MySQL nor SQL Server has CREATE INDEX IF NOT EXISTS, so
            // the existing index names are read back first.
            for(const auto& index : backend_->inspect_table(model.table_name).indexes)
            {
                existing_indexes.push_back(index.name);
            }
        }

        for(const auto& index : create_index_statements(model))
        {
            if(!existing_indexes.empty())
            {
                // The index name is the second token group of the generated SQL.
                if(std::ranges::any_of(existing_indexes, [&](const std::string& name) {
                       return index.sql.find(detail::quote_identifier(name)) != std::string::npos;
                   }))
                {
                    continue;
                }
            }

            backend_->execute(index);
        }
    }

    template <typename Model>
    void migrate_versioned(std::string_view id)
    {
        auto model = describe_model<Model>(backend_->target_dialect());
        auto plan = plan_migration(model, backend_->table_columns(model.table_name));
        entityfy::apply_migration(*backend_, migration{
            .id = std::string{id},
            .statements = std::move(plan.statements),
            .transactional = transaction_depth_ == 0,
        });
        entityfy::require_schema(model, backend_->inspect_table(model.table_name));
    }

    template <typename Model>
    void migrate_versioned()
    {
        migrate_versioned<Model>(table_name<Model>() + "_schema");
    }

    void apply_migrations(const std::vector<migration>& migrations)
    {
        if(transaction_depth_ == 0)
        {
            entityfy::apply_migrations(*backend_, migrations);
            return;
        }

        auto nested_migrations = migrations;
        for(auto& current: nested_migrations)
        {
            current.transactional = false;
        }

        entityfy::apply_migrations(*backend_, nested_migrations);
    }

    template <typename Model>
    void sync_schema()
    {
        this->template table<Model>().sync_schema();
    }

    template <typename Model>
    [[nodiscard]] schema_validation_result validate_schema(schema_validation_options options = {})
    {
        return this->template table<Model>().validate_schema(std::move(options));
    }

    template <typename Model>
    void require_schema(schema_validation_options options = {})
    {
        this->template table<Model>().require_schema(std::move(options));
    }

    template <typename Model>
    [[nodiscard]] insert_result insert(const Model& model)
    {
        return this->template table<Model>().insert(model);
    }

    template <typename Model>
    [[nodiscard]] insert_result create(const Model& model)
    {
        return this->template table<Model>().create(model);
    }

    template <typename Model>
    [[nodiscard]] std::uint64_t update(const Model& model)
    {
        return this->template table<Model>().update(model);
    }

    template <typename Model, typename... Members>
    [[nodiscard]] std::uint64_t update_partial(const Model& model, Members Model::*... members)
    {
        return this->template table<Model>().update_partial(model, members...);
    }

    template <typename Model>
    [[nodiscard]] std::vector<Model> find_many(condition filter = {})
    {
        return this->template table<Model>().find_many(std::move(filter));
    }

    template <typename Model>
    [[nodiscard]] std::vector<Model> find_many(query_options<Model> options)
    {
        return this->template table<Model>().find_many(std::move(options));
    }

    template <typename Model>
    [[nodiscard]] std::vector<Model> find_many(const query_builder<Model>& query)
    {
        return this->template table<Model>().find_many(query);
    }

    template <typename Model>
    [[nodiscard]] std::optional<Model> find_one(condition filter = {})
    {
        return this->template table<Model>().find_one(std::move(filter));
    }

    template <typename Model>
    [[nodiscard]] std::optional<Model> find_one(query_options<Model> options)
    {
        return this->template table<Model>().find_one(std::move(options));
    }

    template <typename Model>
    [[nodiscard]] std::optional<Model> find_one(const query_builder<Model>& query)
    {
        return this->template table<Model>().find_one(query);
    }

    template <typename Model>
    [[nodiscard]] std::uint64_t count(condition filter = {})
    {
        return this->template table<Model>().count(std::move(filter));
    }

    template <typename Model>
    [[nodiscard]] std::uint64_t count(query_options<Model> options)
    {
        return this->template table<Model>().count(std::move(options));
    }

    template <typename Model>
    [[nodiscard]] bool exists(condition filter = {})
    {
        return this->template table<Model>().exists(std::move(filter));
    }

    template <typename Model>
    [[nodiscard]] bool exists(query_options<Model> options)
    {
        return this->template table<Model>().exists(std::move(options));
    }

    template <typename Model>
    [[nodiscard]] insert_result insert_many(const std::vector<Model>& models)
    {
        return this->template table<Model>().insert_many(models);
    }

    template <typename Model>
    [[nodiscard]] insert_result create_many(const std::vector<Model>& models)
    {
        return this->template table<Model>().create_many(models);
    }

    template <typename Model>
    [[nodiscard]] insert_result upsert(const Model& model)
    {
        return this->template table<Model>().upsert(model);
    }

    template <typename Model>
    [[nodiscard]] std::uint64_t update_many(const Model& patch, condition filter)
    {
        return this->template table<Model>().update_many(patch, std::move(filter));
    }

    template <typename Model>
    [[nodiscard]] std::uint64_t update_many(const Model& patch, query_options<Model> options)
    {
        return this->template table<Model>().update_many(patch, std::move(options));
    }

    // General key API: mirrors table_client.
    template <typename Model>
    [[nodiscard]] std::optional<Model> find(const row_key& key)
    {
        return this->template table<Model>().find(key);
    }

    template <typename Model>
    [[nodiscard]] std::uint64_t remove_by_key(const row_key& key)
    {
        return this->template table<Model>().remove_by_key(key);
    }

    template <typename Model, typename Id>
        requires(!std::same_as<std::remove_cvref_t<Id>, row_key>)
    [[nodiscard]] std::optional<Model> find(Id&& id)
    {
        return this->template table<Model>().find(std::forward<Id>(id));
    }

    template <typename Model, typename Id>
        requires(!std::same_as<std::remove_cvref_t<Id>, row_key>)
    [[nodiscard]] std::optional<Model> find_unique(Id&& id)
    {
        return this->template table<Model>().find_unique(std::forward<Id>(id));
    }

    template <typename Model>
    [[nodiscard]] std::uint64_t remove(condition filter)
    {
        return this->template table<Model>().remove(std::move(filter));
    }

    template <typename Model>
    [[nodiscard]] std::uint64_t delete_many(condition filter)
    {
        return this->template table<Model>().delete_many(std::move(filter));
    }

    template <typename Model>
    [[nodiscard]] std::uint64_t delete_many(query_options<Model> options)
    {
        return this->template table<Model>().delete_many(std::move(options));
    }

    template <typename Model>
    [[nodiscard]] std::uint64_t delete_all()
    {
        return this->template table<Model>().delete_all();
    }

    template <typename Model, typename Id>
        requires(!std::same_as<std::remove_cvref_t<Id>, row_key>)
    [[nodiscard]] std::uint64_t remove_by_id(Id&& id)
    {
        return this->template table<Model>().remove_by_id(std::forward<Id>(id));
    }

    [[nodiscard]] detail::execution_result execute(const statement& input)
    {
        return backend_->execute(input);
    }

    [[nodiscard]] std::vector<detail::row> query(const statement& input)
    {
        return backend_->query(input);
    }

    // Streams rows without collecting them; the consumer owns what it keeps and
    // returns false to stop. See detail::backend::query_each.
    void query_each(const statement& input, const std::function<bool(const detail::row&)>& consumer)
    {
        backend_->query_each(input, consumer);
    }

    // Asynchronous counterparts: the same statements travel through the
    // backend's own async path, so SQL generation and key work stay shared. A
    // backend without async support throws the backend's logic_error instead of
    // falling back to a synchronous call.
    [[nodiscard]] task<detail::execution_result> execute_async(const statement& input)
    {
        return backend_->execute_async(input);
    }

    [[nodiscard]] task<std::vector<detail::row>> query_async(const statement& input)
    {
        return backend_->query_async(input);
    }

    [[nodiscard]] task<std::vector<detail::execution_result>> execute_batch_async(
        const std::vector<statement>& input)
    {
        return backend_->execute_batch_async(input);
    }

    // One asynchronous query, then the consumer sees its rows in order. The
    // batch is already fully loaded when the consumer runs, so a half-loaded row
    // is never handed out; returning false stops before the remaining rows.
    [[nodiscard]] task<void> query_each_async(const statement& input,
                                              std::function<bool(const detail::row&)> consumer)
    {
        return query_async(input).then([consumer = std::move(consumer)](std::vector<detail::row> rows) {
            for(const auto& current: rows)
            {
                if(!consumer(current))
                {
                    break;
                }
            }
        });
    }

    // Prepared statement cache. The SQLite backend keeps compiled statements
    // around (least recently used eviction); backends without statement reuse
    // report a capacity of zero.
    void set_statement_cache_capacity(std::size_t capacity)
    {
        backend_->set_statement_cache_capacity(capacity);
    }

    [[nodiscard]] std::size_t statement_cache_capacity() const noexcept
    {
        return backend_->statement_cache_capacity();
    }

    [[nodiscard]] std::size_t statement_cache_size() const noexcept
    {
        return backend_->statement_cache_size();
    }

    void clear_statement_cache()
    {
        backend_->clear_statement_cache();
    }

    // Maximum number of bind parameters a single statement may use. Batch
    // writers (insert_many) split their work by this limit.
    void set_batch_parameter_limit(std::size_t limit)
    {
        backend_->set_batch_parameter_limit(limit);
    }

    [[nodiscard]] std::size_t batch_parameter_limit() const noexcept
    {
        return backend_->batch_parameter_limit();
    }

    // Logs every statement before it is executed.
    void set_logger(std::function<void(std::string_view)> logger)
    {
        backend_->set_logger(std::move(logger));
    }

private:
    void begin_transaction(bool nested, std::string_view savepoint)
    {
        const bool tsql = backend_->target_dialect() == dialect::sqlserver;

        statement command;
        if(tsql)
        {
            command.sql = nested ? "SAVE TRANSACTION " + detail::quote_identifier(savepoint) : "BEGIN TRANSACTION";
        }
        else
        {
            command.sql = nested ? "SAVEPOINT " + detail::quote_identifier(savepoint) : "BEGIN";
        }

        backend_->execute(command);
    }

    void commit_transaction(bool nested, std::string_view savepoint)
    {
        if(backend_->target_dialect() == dialect::sqlserver)
        {
            // T-SQL savepoints are not released; the outer COMMIT finishes them.
            if(nested)
            {
                return;
            }

            backend_->execute(statement{.sql = "COMMIT"});
            return;
        }

        statement command;
        command.sql = nested ? "RELEASE SAVEPOINT " + detail::quote_identifier(savepoint) : "COMMIT";
        backend_->execute(command);
    }

    void rollback_transaction(bool nested, std::string_view savepoint)
    {
        try
        {
            if(backend_->target_dialect() == dialect::sqlserver)
            {
                statement command;
                command.sql = nested ? "ROLLBACK TRANSACTION " + detail::quote_identifier(savepoint) : "ROLLBACK";
                backend_->execute(command);
                return;
            }

            statement command;
            command.sql = nested ? "ROLLBACK TO SAVEPOINT " + detail::quote_identifier(savepoint) : "ROLLBACK";
            backend_->execute(command);

            if(nested)
            {
                command.sql = "RELEASE SAVEPOINT " + detail::quote_identifier(savepoint);
                backend_->execute(command);
            }
        }
        catch(const std::exception& error)
        {
            throw transaction_error{"entityfy transaction rollback failed: " + std::string{error.what()}};
        }
    }

    std::unique_ptr<detail::backend> backend_;
    std::size_t                      transaction_depth_ = 0;
};

namespace detail
{

template <typename Base, typename Type, typename... Types>
void append_type_rows(client& db, std::vector<std::variant<Types...>>& rows)
{
    static_assert(std::is_base_of_v<Base, Type>, "entityfy ORM TPH reads require types derived from the base");

    if(entityfy::table_name<Type>() != entityfy::table_name<Base>())
    {
        throw std::logic_error{
            "entityfy ORM TPH reads require every type to share the base table; check [[= entityfy::table{...}]]"
        };
    }

    for(auto& row : db.template find_many<Type>())
    {
        rows.emplace_back(std::move(row));
    }
}

} // namespace detail

// TPH: reads every concrete type of a hierarchy and returns the instances as
// variants. Each type is one query scoped by its discriminator, in the order the
// types are listed. The rows are plain values, not tracked entities, and eager
// loading is not part of this path.
template <typename Base, typename... Types>
[[nodiscard]] std::vector<std::variant<Types...>> find_all_types(client& db)
{
    static_assert(sizeof...(Types) > 0, "entityfy::find_all_types needs at least one concrete type");

    std::vector<std::variant<Types...>> rows;
    (detail::append_type_rows<Base, Types>(db, rows), ...);
    return rows;
}

} // namespace entityfy
