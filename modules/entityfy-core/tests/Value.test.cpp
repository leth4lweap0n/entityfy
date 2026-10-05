#include <catch2/catch_test_macros.hpp>

#include <entityfy/entityfy.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

TEST_CASE("chrono values bind and materialize as SQL text", "[value]")
{
    const auto day = std::chrono::sys_days{std::chrono::year{2026} / 5 / 20};
    const auto date_value = entityfy::detail::to_sql_value(day);

    REQUIRE(std::get<std::string>(date_value) == "2026-05-20");

    std::chrono::sys_days parsed{};
    entityfy::detail::assign_sql_value(date_value, parsed);
    REQUIRE(parsed == day);

    const auto seconds = std::chrono::hours{15} + std::chrono::minutes{30} + std::chrono::seconds{5};
    const auto time_value = entityfy::detail::to_sql_value(seconds);
    REQUIRE(std::get<std::string>(time_value) == "15:30:05");
}

TEST_CASE("annotation text is structural and viewable", "[annotations]")
{
    constexpr entityfy::table table{"users"};
    static_assert(table.name.view() == "users");
    REQUIRE(table.name.view() == "users");
}

TEST_CASE("materialization checks narrow integer overflow", "[value]")
{
    std::int8_t output = 0;
    REQUIRE_THROWS_AS(
        entityfy::detail::assign_sql_value(
            entityfy::sql_value{static_cast<std::int64_t>(std::numeric_limits<std::int8_t>::max()) + 1},
            output
        ),
        std::overflow_error
    );
}
