#pragma once

#include <entityfy/entityfy.hpp>
#include <entityfy/migrate/schema.hpp>

#include <string>
#include <utility>

namespace entityfy::migrate
{

// Provider-independent schema output produced from a sample C++ model definition.
// SQL types are produced by entityfy::describe_model<T>(dialect).
template <typename Model>
[[nodiscard]] table table_from_model(entityfy::dialect target)
{
    const auto descriptor = entityfy::describe_model<Model>(target);

    table output;
    output.name = descriptor.table_name;

    // Declaration order, straight from the descriptor's key indices.
    for(const auto& source: entityfy::primary_key_columns(descriptor))
    {
        output.primary_key.push_back(source.name);
    }

    for(const auto& source: descriptor.columns)
    {
        column destination;
        destination.name = source.name;
        destination.type = source.sql_type;
        destination.not_null = source.flags.not_null || source.flags.primary_key;
        destination.primary_key = source.flags.primary_key;
        destination.auto_increment = source.flags.auto_increment;
        destination.unique = source.flags.unique;
        destination.indexed = source.flags.indexed;
        destination.default_sql = source.default_sql;
        destination.check_sql = source.check_sql;

        if(source.has_reference && source.reference.group == 0)
        {
            destination.references = reference{
                .table = source.reference.table,
                .column = source.reference.column.empty() ? std::string{"id"} : source.reference.column,
                .on_delete = source.reference.on_delete,
                .on_update = source.reference.on_update,
            };
        }

        if(destination.unique || destination.indexed)
        {
            output.indexes.push_back(index{
                .name = (destination.unique ? "uidx_" : "idx_") + output.name + "_" + destination.name,
                .columns = {destination.name},
                .unique = destination.unique,
            });
        }

        output.columns.emplace_back(std::move(destination));
    }

    for(const auto& relation: descriptor.relations)
    {
        foreign_key key;
        key.columns = relation.local_columns.empty() ? std::vector<std::string>{relation.local_column}
                                                     : relation.local_columns;
        key.ref_columns = relation.referenced_columns.empty()
                              ? std::vector<std::string>{relation.referenced_column.empty() ? "id"
                                                                                          : relation.referenced_column}
                              : relation.referenced_columns;

        key.column = key.columns.front();
        key.ref_table = relation.referenced_table;
        key.ref_column = key.ref_columns.front();
        key.on_delete = relation.on_delete;
        key.on_update = relation.on_update;

        output.foreign_keys.emplace_back(std::move(key));
    }

    return output;
}

template <typename... Models>
[[nodiscard]] schema schema_from_models(entityfy::dialect target)
{
    schema output;
    (output.tables.push_back(table_from_model<Models>(target)), ...);
    sort_schema(output);
    return output;
}

} // namespace entityfy::migrate
