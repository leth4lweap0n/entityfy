#pragma once

#include <entityfy/entityfy.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

namespace entityfy::test
{

struct [[= entityfy::table{"users"}]] User
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::unique, = entityfy::indexed, = entityfy::not_null, = entityfy::varchar{320}]]
    std::string email;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;

    [[= entityfy::created_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point created_at{};

    [[= entityfy::updated_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point updated_at{};
};

struct [[= entityfy::table{"posts"}]] Post
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"users", "id", "CASCADE", "CASCADE"}]]
    std::int64_t user_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{200}]]
    std::string title;
};

inline const column_descriptor& column_by_name(const model_descriptor<User>& model, std::string_view name)
{
    const auto found = std::ranges::find_if(model.columns, [&](const column_descriptor& column) {
        return column.name == name;
    });

    if(found == model.columns.end())
    {
        throw std::logic_error{"missing fixture column"};
    }

    return *found;
}

} // namespace entityfy::test
