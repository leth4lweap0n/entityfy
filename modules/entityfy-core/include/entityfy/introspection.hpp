#pragma once

#include <string>
#include <vector>

namespace entityfy
{

struct column_info
{
    std::string name{};
    std::string sql_type{};
    bool        nullable = true;
    bool        primary_key = false;
    // Position of the column inside a composite primary key: 0 means the column
    // is not part of the key, 1..N is the position in the key. Backends that
    // cannot report the position leave it 0 and only set `primary_key`.
    int         primary_key_ordinal = 0;
    std::string default_sql{};
};

struct index_info
{
    std::string              name{};
    bool                     unique = false;
    std::vector<std::string> columns{};
};

struct foreign_key_info
{
    // First entry of `columns`; kept for call sites that only look at one
    // column of the key.
    std::string column{};
    std::string referenced_table{};
    // First entry of `referenced_columns`, matching `column`.
    std::string referenced_column{};
    std::string on_update{};
    std::string on_delete{};
    // Local and referenced columns of the whole key in key order. A
    // single-column key has one-element lists. Providers that cannot report
    // key positions leave both empty; consumers then fall back to `column` and
    // `referenced_column`.
    std::vector<std::string> columns{};
    std::vector<std::string> referenced_columns{};
};

struct table_info
{
    std::string                   name{};
    std::vector<column_info>      columns{};
    std::vector<index_info>       indexes{};
    std::vector<foreign_key_info> foreign_keys{};

    [[nodiscard]] bool exists() const noexcept
    {
        return !columns.empty();
    }
};

} // namespace entityfy
