#pragma once

#include <entityfy/migrate/schema.hpp>

#include <map>
#include <string>
#include <vector>

namespace entityfy::migrate
{

enum class op_kind
{
    create_table,
    drop_table,
    add_column,
    drop_column,
    rename_column,
    alter_column,
    add_index,
    drop_index,
    add_foreign_key,
    drop_foreign_key,
};

struct operation
{
    op_kind     kind{};
    std::string table;
    std::string column;
    entityfy::migrate::column column_spec;
    std::string old_name;
    std::string new_name;
    index       index_spec;
    foreign_key fk_spec;
    bool        destructive = false;
    std::string detail;
};

struct diff_options
{
    // "table.old_column" -> "new_column"; without it a rename appears as drop + add.
    std::map<std::string, std::string, std::less<>> renames;

    // Some providers cannot read check constraints from the schema; this disables that comparison.
    bool ignore_checks = false;
};

[[nodiscard]] std::vector<operation> diff_schemas(
    const schema& desired,
    const schema& actual,
    const diff_options& options = {});

[[nodiscard]] bool has_destructive(const std::vector<operation>& operations);
[[nodiscard]] bool has_only_additive(const std::vector<operation>& operations);
[[nodiscard]] std::string describe_operation(const operation& value);

} // namespace entityfy::migrate
