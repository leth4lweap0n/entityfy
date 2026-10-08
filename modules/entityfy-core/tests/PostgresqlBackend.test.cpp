#include <catch2/catch_test_macros.hpp>

#include <entityfy/detail/backend.hpp>

#include <string>

TEST_CASE("postgres placeholders rewrite generated question-mark binds", "[postgresql][statement]")
{
    REQUIRE(
        entityfy::detail::postgres_placeholders(
            "SELECT * FROM \"users\" WHERE \"email\" = ? AND \"id\" = ?"
        ) == "SELECT * FROM \"users\" WHERE \"email\" = $1 AND \"id\" = $2"
    );
}

TEST_CASE("postgres placeholders preserve native dollar binds", "[postgresql][statement]")
{
    REQUIRE(
        entityfy::detail::postgres_placeholders(
            "SELECT * FROM \"posts\" WHERE \"metadata\" ? 'tags' AND \"id\" = $1"
        ) == "SELECT * FROM \"posts\" WHERE \"metadata\" ? 'tags' AND \"id\" = $1"
    );
}

TEST_CASE("postgres placeholders ignore dollar binds inside quoted SQL", "[postgresql][statement]")
{
    REQUIRE(
        entityfy::detail::postgres_placeholders(
            "SELECT '$1', $$?$$, \"metadata\" ? 'tags' WHERE \"id\" = ?"
        ) == "SELECT '$1', $$?$$, \"metadata\" $1 'tags' WHERE \"id\" = $2"
    );
}
