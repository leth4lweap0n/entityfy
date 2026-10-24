#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace entityfy::migrate
{

struct reference
{
    std::string table;
    std::string column{"id"};
    std::string on_delete;
    std::string on_update;
};

struct column
{
    std::string           name;
    std::string           type;
    bool                  not_null = false;
    bool                  primary_key = false;
    bool                  auto_increment = false;
    bool                  unique = false;
    bool                  indexed = false;
    std::string           default_sql;
    std::string           check_sql;
    std::optional<reference> references;

    [[nodiscard]] bool is_nullable() const noexcept
    {
        return !not_null && !primary_key;
    }
};

struct index
{
    std::string              name;
    std::vector<std::string> columns;
    bool                     unique = false;
};

struct foreign_key
{
    // First local column; `columns` carries every part of the constraint.
    // The singular members mirror the first list entries so snapshots and
    // hand-written schemas from before composite support keep working.
    std::string column;
    std::string ref_table;
    std::string ref_column{"id"};
    std::string on_delete;
    std::string on_update;

    // Every local/referenced column in declaration order. A single-column
    // foreign key has one-element lists; two or more entries describe one
    // composite table-level constraint, never one constraint per column.
    std::vector<std::string> columns;
    std::vector<std::string> ref_columns;
};

// Ordered local/referenced columns, falling back to the singular mirror for a
// foreign key that was built without the lists.
[[nodiscard]] inline std::vector<std::string> foreign_key_columns(const foreign_key& value)
{
    if(!value.columns.empty())
    {
        return value.columns;
    }

    return value.column.empty() ? std::vector<std::string>{} : std::vector<std::string>{value.column};
}

[[nodiscard]] inline std::vector<std::string> foreign_key_ref_columns(const foreign_key& value)
{
    if(!value.ref_columns.empty())
    {
        return value.ref_columns;
    }

    return value.ref_column.empty() ? std::vector<std::string>{} : std::vector<std::string>{value.ref_column};
}

struct table
{
    std::string             name;
    std::vector<column>     columns;
    std::vector<index>      indexes;
    std::vector<foreign_key> foreign_keys;

    // Ordered primary-key column names. An empty list means "read the
    // per-column flags in column order", which keeps snapshots written before
    // this field existed readable.
    std::vector<std::string> primary_key;
};

struct schema
{
    std::vector<table> tables;
};

// Providers that cannot express a composite foreign key call this before
// planning; emitting one constraint per column would silently change the
// schema. Throws std::logic_error naming the provider, table and columns.
inline void require_single_column_foreign_keys(const schema& value, std::string_view provider_name)
{
    for(const auto& current: value.tables)
    {
        for(const auto& key: current.foreign_keys)
        {
            const auto columns = foreign_key_columns(key);
            if(columns.size() < 2)
            {
                continue;
            }

            std::string listed;
            for(const auto& name: columns)
            {
                if(!listed.empty())
                {
                    listed += ", ";
                }

                listed += name;
            }

            throw std::logic_error{"entityfy/migrate:" + std::string{provider_name} +
                                   ": composite foreign keys are not supported; table `" + current.name +
                                   "` declares (" + listed + ")"};
        }
    }
}

[[nodiscard]] const table*  find_table(const schema& value, std::string_view name);
[[nodiscard]] table*        find_table(schema& value, std::string_view name);
[[nodiscard]] const column* find_column(const table& value, std::string_view name);
[[nodiscard]] column*       find_column(table& value, std::string_view name);
[[nodiscard]] const index*  find_index(const table& value, std::string_view name);

// Ordered key columns: `table::primary_key` when present, otherwise the
// columns flagged `primary_key` in declaration order.
[[nodiscard]] std::vector<std::string> primary_key_columns(const table& value);

// For comparing SQL text: uppercased and whitespace removed.
[[nodiscard]] std::string normalize_sql(std::string_view input);
[[nodiscard]] bool        same_type(std::string_view left, std::string_view right);
[[nodiscard]] bool        same_default(std::string_view left, std::string_view right);

// Deep, order-independent comparison (table/column/index names are the key).
[[nodiscard]] bool same_schema(const schema& left, const schema& right);

// Sort tables and indexes by name so JSON and diff output stay deterministic.
void sort_schema(schema& value);

[[nodiscard]] std::string quote_ident(std::string_view identifier);
[[nodiscard]] std::string quote_literal(std::string_view value);

} // namespace entityfy::migrate
