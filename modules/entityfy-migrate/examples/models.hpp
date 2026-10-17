#pragma once

#include <entityfy/migrate/model.hpp>

#include <chrono>
#include <cstdint>
#include <string>

struct [[= entityfy::table{"users"}]] User
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
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
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"users", "id", "CASCADE"}]]
    std::int64_t user_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{200}]]
    std::string title;

    [[= entityfy::text]]
    std::string body;

    [[= entityfy::not_null]]
    bool published = false;

    [[= entityfy::created_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point created_at{};
};

inline entityfy::migrate::schema current_schema(entityfy::dialect target)
{
    return entityfy::migrate::schema_from_models<User, Post>(target);
}
