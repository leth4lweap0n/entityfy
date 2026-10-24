#include <entityfy/migrate/diff.hpp>

#include <algorithm>
#include <set>
#include <sstream>

namespace entityfy::migrate
{
namespace
{

bool foreign_keys_equal(const foreign_key& left, const foreign_key& right)
{
    return foreign_key_columns(left) == foreign_key_columns(right) &&
           foreign_key_ref_columns(left) == foreign_key_ref_columns(right) &&
           left.ref_table == right.ref_table &&
           normalize_sql(left.on_delete) == normalize_sql(right.on_delete) &&
           normalize_sql(left.on_update) == normalize_sql(right.on_update);
}

std::string describe_foreign_key(const table& value, const foreign_key& key)
{
    std::string output = value.name + " (";

    const auto columns = foreign_key_columns(key);
    for(std::size_t index = 0; index < columns.size(); ++index)
    {
        if(index != 0)
        {
            output += ", ";
        }

        output += columns[index];
    }

    output += ") -> " + key.ref_table + " (";

    const auto referenced = foreign_key_ref_columns(key);
    for(std::size_t index = 0; index < referenced.size(); ++index)
    {
        if(index != 0)
        {
            output += ", ";
        }

        output += referenced[index];
    }

    output += ")";
    return output;
}

bool references_equal(const column& left, const column& right)
{
    if(left.references.has_value() != right.references.has_value())
    {
        return false;
    }

    if(!left.references)
    {
        return true;
    }

    return left.references->table == right.references->table &&
           left.references->column == right.references->column &&
           normalize_sql(left.references->on_delete) == normalize_sql(right.references->on_delete) &&
           normalize_sql(left.references->on_update) == normalize_sql(right.references->on_update);
}

bool column_differs(const column& desired, const column& actual, bool ignore_checks, bool& type_changed)
{
    type_changed = !same_type(desired.type, actual.type);

    const bool check_differs = !ignore_checks &&
                               normalize_sql(desired.check_sql) != normalize_sql(actual.check_sql);

    return type_changed || desired.not_null != actual.not_null || desired.primary_key != actual.primary_key ||
           desired.auto_increment != actual.auto_increment ||
           !same_default(desired.default_sql, actual.default_sql) || check_differs ||
           !references_equal(desired, actual);
}

} // namespace

std::vector<operation> diff_schemas(const schema& desired, const schema& actual, const diff_options& options)
{
    std::vector<operation> operations;

    for(const auto& desired_table: desired.tables)
    {
        if(find_table(actual, desired_table.name) == nullptr)
        {
            operation value;
            value.kind = op_kind::create_table;
            value.table = desired_table.name;
            value.detail = "create table " + desired_table.name;
            operations.emplace_back(std::move(value));
        }
    }

    for(const auto& actual_table: actual.tables)
    {
        if(find_table(desired, actual_table.name) == nullptr)
        {
            operation value;
            value.kind = op_kind::drop_table;
            value.table = actual_table.name;
            value.destructive = true;
            value.detail = "drop table " + actual_table.name;
            operations.emplace_back(std::move(value));
        }
    }

    for(const auto& desired_table: desired.tables)
    {
        const table* actual_table = find_table(actual, desired_table.name);
        if(actual_table == nullptr)
        {
            continue;
        }

        std::set<std::string> consumed_renames;

        for(const auto& desired_column: desired_table.columns)
        {
            const column* actual_column = find_column(*actual_table, desired_column.name);
            if(actual_column != nullptr)
            {
                bool type_changed = false;
                if(column_differs(desired_column, *actual_column, options.ignore_checks, type_changed))
                {
                    operation value;
                    value.kind = op_kind::alter_column;
                    value.table = desired_table.name;
                    value.column = desired_column.name;
                    value.column_spec = desired_column;
                    value.destructive = type_changed || desired_column.primary_key != actual_column->primary_key;
                    value.detail = std::string{"alter column "} + desired_table.name + "." + desired_column.name +
                                   (type_changed ? " (type " + actual_column->type + " -> " + desired_column.type + ")"
                                                 : " (constraints)");
                    operations.emplace_back(std::move(value));
                }
                continue;
            }

            bool renamed = false;
            for(const auto& [key, target]: options.renames)
            {
                if(target != desired_column.name)
                {
                    continue;
                }

                const auto separator = key.find('.');
                if(separator == std::string::npos || key.substr(0, separator) != desired_table.name)
                {
                    continue;
                }

                const std::string old_name = key.substr(separator + 1);
                if(find_column(*actual_table, old_name) != nullptr && find_column(desired_table, old_name) == nullptr)
                {
                    operation value;
                    value.kind = op_kind::rename_column;
                    value.table = desired_table.name;
                    value.old_name = old_name;
                    value.new_name = desired_column.name;
                    value.column_spec = desired_column;
                    value.detail = "rename column " + desired_table.name + "." + old_name + " -> " + desired_column.name;
                    operations.emplace_back(std::move(value));
                    consumed_renames.insert(old_name);
                    renamed = true;
                    break;
                }
            }

            if(!renamed)
            {
                operation value;
                value.kind = op_kind::add_column;
                value.table = desired_table.name;
                value.column = desired_column.name;
                value.column_spec = desired_column;
                value.detail = "add column " + desired_table.name + "." + desired_column.name;
                operations.emplace_back(std::move(value));
            }
        }

        for(const auto& actual_column: actual_table->columns)
        {
            if(find_column(desired_table, actual_column.name) != nullptr)
            {
                continue;
            }

            if(consumed_renames.contains(actual_column.name))
            {
                continue;
            }

            operation value;
            value.kind = op_kind::drop_column;
            value.table = desired_table.name;
            value.column = actual_column.name;
            value.destructive = true;
            value.detail = "drop column " + desired_table.name + "." + actual_column.name;
            operations.emplace_back(std::move(value));
        }

        for(const auto& desired_index: desired_table.indexes)
        {
            const index* actual_index = find_index(*actual_table, desired_index.name);
            if(actual_index == nullptr)
            {
                operation value;
                value.kind = op_kind::add_index;
                value.table = desired_table.name;
                value.index_spec = desired_index;
                value.detail = "create index " + desired_index.name;
                operations.emplace_back(std::move(value));
                continue;
            }

            if(actual_index->unique != desired_index.unique || actual_index->columns != desired_index.columns)
            {
                operation drop;
                drop.kind = op_kind::drop_index;
                drop.table = desired_table.name;
                drop.index_spec = *actual_index;
                drop.detail = "drop index " + actual_index->name;
                operations.emplace_back(std::move(drop));

                operation add;
                add.kind = op_kind::add_index;
                add.table = desired_table.name;
                add.index_spec = desired_index;
                add.detail = "create index " + desired_index.name;
                operations.emplace_back(std::move(add));
            }
        }

        for(const auto& actual_index: actual_table->indexes)
        {
            if(find_index(desired_table, actual_index.name) == nullptr)
            {
                operation value;
                value.kind = op_kind::drop_index;
                value.table = desired_table.name;
                value.index_spec = actual_index;
                value.detail = "drop index " + actual_index.name;
                operations.emplace_back(std::move(value));
            }
        }

        for(const auto& desired_fk: desired_table.foreign_keys)
        {
            const auto found = std::ranges::find_if(actual_table->foreign_keys, [&](const foreign_key& candidate) {
                return foreign_keys_equal(candidate, desired_fk);
            });

            if(found == actual_table->foreign_keys.end())
            {
                operation value;
                value.kind = op_kind::add_foreign_key;
                value.table = desired_table.name;
                value.fk_spec = desired_fk;
                value.detail = "add foreign key " + describe_foreign_key(desired_table, desired_fk);
                operations.emplace_back(std::move(value));
            }
        }

        for(const auto& actual_fk: actual_table->foreign_keys)
        {
            const auto found = std::ranges::find_if(desired_table.foreign_keys, [&](const foreign_key& candidate) {
                return foreign_keys_equal(candidate, actual_fk);
            });

            if(found == desired_table.foreign_keys.end())
            {
                operation value;
                value.kind = op_kind::drop_foreign_key;
                value.table = desired_table.name;
                value.fk_spec = actual_fk;
                value.detail = "drop foreign key " + describe_foreign_key(desired_table, actual_fk);
                operations.emplace_back(std::move(value));
            }
        }
    }

    return operations;
}

bool has_destructive(const std::vector<operation>& operations)
{
    return std::ranges::any_of(operations, [](const operation& value) {
        return value.destructive;
    });
}

bool has_only_additive(const std::vector<operation>& operations)
{
    return !has_destructive(operations);
}

std::string describe_operation(const operation& value)
{
    return value.detail.empty() ? "operation" : value.detail;
}

} // namespace entityfy::migrate
