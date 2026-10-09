#include "support/Fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("schema validation reports matching table as valid", "[schema][validation]")
{
    const auto model = entityfy::describe_model<entityfy::test::User>(entityfy::dialect::sqlite);

    entityfy::table_info table{
        .name = "users",
        .columns = {
            entityfy::column_info{.name = "id", .sql_type = "INTEGER", .nullable = false, .primary_key = true},
            entityfy::column_info{.name = "email", .sql_type = "VARCHAR(320)", .nullable = false},
            entityfy::column_info{.name = "name", .sql_type = "VARCHAR(120)", .nullable = false},
            entityfy::column_info{.name = "created_at", .sql_type = "TEXT", .nullable = false, .default_sql = "CURRENT_TIMESTAMP"},
            entityfy::column_info{.name = "updated_at", .sql_type = "TEXT", .nullable = false, .default_sql = "CURRENT_TIMESTAMP"},
        },
        .indexes = {
            entityfy::index_info{.name = "uidx_users_email", .unique = true, .columns = {"email"}},
        },
    };

    REQUIRE(entityfy::validate_schema(model, table).valid());
}

TEST_CASE("schema validation reports drift", "[schema][validation]")
{
    const auto model = entityfy::describe_model<entityfy::test::User>(entityfy::dialect::sqlite);

    entityfy::table_info table{
        .name = "users",
        .columns = {
            entityfy::column_info{.name = "id", .sql_type = "INTEGER", .nullable = false, .primary_key = true},
            entityfy::column_info{.name = "email", .sql_type = "INTEGER", .nullable = true},
            entityfy::column_info{.name = "legacy_only", .sql_type = "TEXT", .nullable = true},
        },
    };

    const auto result = entityfy::validate_schema(model, table);
    REQUIRE_FALSE(result.valid());
    REQUIRE(result.issues.size() >= 4);
}
