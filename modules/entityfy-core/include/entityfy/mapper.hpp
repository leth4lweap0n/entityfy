#pragma once

#include <entityfy/meta.hpp>
#include <entityfy/schema.hpp>
#include <entityfy/value.hpp>

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

    meta::for_each_persistent_field<ignore_t>(model, [&](meta::field meta_field, auto& value) {
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
            throw std::runtime_error{"entityfy ORM failed to read column `" + std::string{meta_field.name} + "`: " + error.what()};
        }

        ++index;
    });

    return model;
}

template <typename Model>
[[nodiscard]] sql_value primary_key_value(const Model& object, const model_descriptor<Model>& model)
{
    std::size_t descriptor_index = 0;
    std::optional<sql_value> output;

    meta::for_each_persistent_field<ignore_t>(object, [&](meta::field, const auto& value) {
        const auto& descriptor = model.columns[descriptor_index];
        if(descriptor.flags.primary_key)
        {
            output = to_sql_value(value);
        }

        ++descriptor_index;
    });

    if(!output)
    {
        throw std::logic_error{"entityfy ORM failed to read primary key value from model"};
    }

    return std::move(*output);
}

} // namespace entityfy::detail
