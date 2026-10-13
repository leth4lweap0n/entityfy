#pragma once

#include <entityfy/migrate/schema.hpp>

#include <string>
#include <vector>

namespace entityfy::migrate
{

struct step
{
    enum class kind
    {
        sql,
        sqlite_rebuild,
    };

    kind        type = kind::sql;
    std::string sql;
    table       desired_table;
    table       actual_table;
    std::string label;
};

struct plan
{
    std::vector<step>        steps;
    bool                     destructive = false;
    std::vector<std::string> warnings;
};

} // namespace entityfy::migrate
