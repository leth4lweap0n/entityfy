#pragma once

#include <entityfy/key.hpp>
#include <entityfy/meta.hpp>
#include <entityfy/schema.hpp>
#include <entityfy/value.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace entityfy::detail
{

template <typename Model>
[[nodiscard]] Model materialize_row(const std::vector<sql_value>& row)
{
    Model       model{};
    std::size_t index = 0;

    detail::for_each_leaf_field(model, [&](std::string_view prefix, std::string_view name, auto& value) {
        if(index >= row.size())
        {
            throw std::runtime_error{"entityfy ORM result row has fewer columns than the model schema"};
        }

        try
        {
            assign_sql_value(row[index], value);
        }
        catch(const std::exception& error)
        {
            std::string column_name{prefix};
            column_name.append(name);
            throw std::runtime_error{"entityfy ORM failed to read column `" + column_name + "`: " + error.what()};
        }

        ++index;
    });

    return model;
}

// Reads the entity's primary key as a row_key, in descriptor order. The
// descriptor indices are the key definition; the flag scan is the fallback for
// a hand-built descriptor.
template <typename Model>
[[nodiscard]] row_key primary_key_of(const Model& object, const model_descriptor<Model>& model)
{
    std::vector<std::size_t> key_indices = model.primary_key_indices;

    if(key_indices.empty())
    {
        for(std::size_t index = 0; index < model.columns.size(); ++index)
        {
            if(model.columns[index].flags.primary_key)
            {
                key_indices.push_back(index);
            }
        }
    }

    std::vector<sql_value> parts;
    parts.reserve(key_indices.size());

    std::size_t next = 0;
    std::size_t descriptor_index = 0;

    detail::for_each_leaf_field(object, [&](std::string_view, std::string_view, const auto& value) {
        if(next < key_indices.size() && key_indices[next] == descriptor_index)
        {
            parts.emplace_back(detail::to_sql_value(value));
            ++next;
        }

        ++descriptor_index;
    });

    if(parts.size() != key_indices.size())
    {
        throw std::logic_error{"entityfy ORM failed to read primary key value from model"};
    }

    return row_key{std::move(parts)};
}

// Single-column key value for relationship helpers (has_many/belongs_to and the
// context's include grouping). Composite keys belong to row_key.
template <typename Model>
[[nodiscard]] sql_value primary_key_value(const Model& object, const model_descriptor<Model>& model)
{
    auto key = primary_key_of(object, model);

    if(key.size() != 1)
    {
        throw std::logic_error{
            "entityfy ORM model has a composite primary key; single-column key helpers need a row_key"
        };
    }

    return key.part(0);
}

} // namespace entityfy::detail
