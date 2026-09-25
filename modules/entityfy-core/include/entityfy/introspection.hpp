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
    std::string column{};
    std::string referenced_table{};
    std::string referenced_column{};
    std::string on_update{};
    std::string on_delete{};
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
