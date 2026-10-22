#include <entityfy/entityfy.hpp>

#include <chrono>
#include <cstdint>
#include <string>

#include <spdlog/spdlog.h>

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

int main()
{
    spdlog::info("started");

    auto db = entityfy::client("sqlite://:memory:");
    auto users = db.table<User>();
    users.migrate();

    const auto ada = db.insert<User>({.email = "ada@test.com", .name = "Ada"});
    const auto grace = users.insert({.email = "grace@test.com", .name = "Grace"});
    spdlog::info("inserted rows: {}, {}", ada.rows_affected, grace.rows_affected);

    const auto matches = users.find_many(
        entityfy::where(&User::email).ends_with("@test.com")
    );

    for(const auto& user: matches)
    {
        spdlog::info("user {}: {} <{}>", user.id, user.name, user.email);
    }
}
