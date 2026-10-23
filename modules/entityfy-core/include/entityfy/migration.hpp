#pragma once

#include <entityfy/detail/backend.hpp>
#include <entityfy/error.hpp>
#include <entityfy/schema.hpp>
#include <entityfy/statement.hpp>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace entityfy
{

struct migration_plan
{
    std::vector<statement> statements;

    [[nodiscard]] bool empty() const noexcept
    {
        return statements.empty();
    }
};

struct migration
{
    std::string            id;
    std::vector<statement> statements;
    bool                   transactional = true;
};

template <typename Model>
[[nodiscard]] migration_plan plan_migration(
    const model_descriptor<Model>& model,
    const std::vector<std::string>& existing_columns
)
{
    migration_plan plan;

    if(existing_columns.empty())
    {
        plan.statements = create_schema_statements(model);
        return plan;
    }

    for(const auto& descriptor: model.columns)
    {
        if(descriptor.flags.system)
        {
            continue;
        }

        const bool exists = std::ranges::any_of(existing_columns, [&](std::string_view existing) {
            return existing == descriptor.name;
        });

        if(!exists)
        {
            plan.statements.emplace_back(add_column(model, descriptor));
        }
    }

    auto indexes = create_index_statements(model);
    std::ranges::move(indexes, std::back_inserter(plan.statements));
    return plan;
}

inline constexpr std::string_view migration_history_table = "entityfy_schema_migrations";

inline statement create_migration_table_statement()
{
    return statement{
        .sql =
            "CREATE TABLE IF NOT EXISTS \"" + std::string{migration_history_table} + "\" ("
            "\"id\" TEXT PRIMARY KEY, "
            "\"applied_at\" TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
            ")",
    };
}

inline statement record_migration_statement(std::string_view id)
{
    return statement{
        .sql = "INSERT INTO \"" + std::string{migration_history_table} + "\" (\"id\") VALUES (?)",
        .binds = {std::string{id}},
    };
}

namespace detail
{

inline void execute_control_statement(backend& backend, std::string_view sql)
{
    backend.execute(statement{.sql = std::string{sql}});
}

inline void rollback_control_statement(backend& backend) noexcept
{
    try
    {
        execute_control_statement(backend, "ROLLBACK");
    }
    catch(...)
    {
    }
}

} // namespace detail

inline std::set<std::string> applied_migration_ids(detail::backend& backend)
{
    backend.execute(create_migration_table_statement());

    const auto rows = backend.query(statement{
        .sql = "SELECT \"id\" FROM \"" + std::string{migration_history_table} + "\" ORDER BY \"id\"",
    });

    std::set<std::string> output;
    for(const auto& row: rows)
    {
        if(!row.empty())
        {
            output.emplace(detail::value_to_string(row.front()));
        }
    }

    return output;
}

inline void apply_migration(detail::backend& backend, const migration& input)
{
    if(input.id.empty())
    {
        throw migration_error{"entityfy migration id cannot be empty"};
    }

    auto applied = applied_migration_ids(backend);
    if(applied.contains(input.id))
    {
        return;
    }

    const auto run_migration_body = [&]() {
        for(const auto& migration_statement: input.statements)
        {
            backend.execute(migration_statement);
        }

        backend.execute(record_migration_statement(input.id));
    };

    bool transaction_started = false;

    try
    {
        if(input.transactional)
        {
            detail::execute_control_statement(backend, "BEGIN");
            transaction_started = true;
            run_migration_body();
            detail::execute_control_statement(backend, "COMMIT");
            transaction_started = false;
        }
        else
        {
            run_migration_body();
        }
    }
    catch(const std::exception& error)
    {
        if(transaction_started)
        {
            detail::rollback_control_statement(backend);
        }

        throw migration_error{"entityfy migration `" + input.id + "` failed: " + error.what()};
    }
}

inline void apply_migrations(detail::backend& backend, const std::vector<migration>& migrations)
{
    for(const auto& current: migrations)
    {
        apply_migration(backend, current);
    }
}

} // namespace entityfy
