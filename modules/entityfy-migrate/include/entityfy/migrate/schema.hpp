#pragma once

#include <optional>
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
    std::string column;
    std::string ref_table;
    std::string ref_column{"id"};
    std::string on_delete;
    std::string on_update;
};

struct table
{
    std::string             name;
    std::vector<column>     columns;
    std::vector<index>      indexes;
    std::vector<foreign_key> foreign_keys;
};

struct schema
{
    std::vector<table> tables;
};

[[nodiscard]] const table*  find_table(const schema& value, std::string_view name);
[[nodiscard]] table*        find_table(schema& value, std::string_view name);
[[nodiscard]] const column* find_column(const table& value, std::string_view name);
[[nodiscard]] column*       find_column(table& value, std::string_view name);
[[nodiscard]] const index*  find_index(const table& value, std::string_view name);

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
