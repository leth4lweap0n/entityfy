#include "support/Fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace
{

// Two explicit key parts: the ordered key set must match declaration order.
struct [[= entityfy::table{"schema_ck_people"}]] schema_pair
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::primary_key, = entityfy::varchar{16}]]
    std::string region;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string title;
};

[[nodiscard]] entityfy::table_info schema_pair_table(int id_ordinal, int region_ordinal)
{
    return entityfy::table_info{
        .name = "schema_ck_people",
        .columns = {
            entityfy::column_info{
                .name = "id",
                .sql_type = "INTEGER",
                .nullable = false,
                .primary_key = id_ordinal != 0,
                .primary_key_ordinal = id_ordinal,
            },
            entityfy::column_info{
                .name = "region",
                .sql_type = "VARCHAR(16)",
                .nullable = false,
                .primary_key = region_ordinal != 0,
                .primary_key_ordinal = region_ordinal,
            },
            entityfy::column_info{.name = "title", .sql_type = "VARCHAR(120)", .nullable = false},
        },
    };
}

} // namespace

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

TEST_CASE("schema validation accepts a matching composite key", "[schema][validation][composite]")
{
    const auto model = entityfy::describe_model<schema_pair>(entityfy::dialect::sqlite);

    const auto result = entityfy::validate_schema(model, schema_pair_table(1, 2));
    REQUIRE(result.valid());
}

TEST_CASE("schema validation reports a reversed composite key", "[schema][validation][composite]")
{
    const auto model = entityfy::describe_model<schema_pair>(entityfy::dialect::sqlite);

    // Same columns, same flags: only the positions in the key differ.
    const auto result = entityfy::validate_schema(model, schema_pair_table(2, 1));

    REQUIRE_FALSE(result.valid());
    REQUIRE(result.issues.size() == 1);

    const auto& issue = result.issues.front();
    REQUIRE(issue.kind == entityfy::schema_issue_kind::primary_key_mismatch);
    REQUIRE(issue.object_name == "schema_ck_people");
    REQUIRE(issue.expected == "id, region");
    REQUIRE(issue.actual == "region, id");
}

TEST_CASE("schema validation reports a composite key that is only one column", "[schema][validation][composite]")
{
    const auto model = entityfy::describe_model<schema_pair>(entityfy::dialect::sqlite);

    const auto result = entityfy::validate_schema(model, schema_pair_table(1, 0));

    REQUIRE_FALSE(result.valid());
    REQUIRE(result.issues.size() == 1);

    const auto& issue = result.issues.front();
    REQUIRE(issue.kind == entityfy::schema_issue_kind::primary_key_mismatch);
    REQUIRE(issue.object_name == "schema_ck_people");
    REQUIRE(issue.expected == "id, region");
    REQUIRE(issue.actual == "id");
}

TEST_CASE("schema validation falls back to per-column keys without ordinals", "[schema][validation]")
{
    const auto model = entityfy::describe_model<entityfy::test::User>(entityfy::dialect::sqlite);

    // Every ordinal is 0, so the provider only exposes the boolean flag and the
    // original per-column message must survive unchanged.
    entityfy::table_info table{
        .name = "users",
        .columns = {
            entityfy::column_info{.name = "id", .sql_type = "INTEGER", .nullable = false},
            entityfy::column_info{.name = "email", .sql_type = "VARCHAR(320)", .nullable = false},
            entityfy::column_info{.name = "name", .sql_type = "VARCHAR(120)", .nullable = false},
            entityfy::column_info{.name = "created_at", .sql_type = "TEXT", .nullable = false, .default_sql = "CURRENT_TIMESTAMP"},
            entityfy::column_info{.name = "updated_at", .sql_type = "TEXT", .nullable = false, .default_sql = "CURRENT_TIMESTAMP"},
        },
        .indexes = {
            entityfy::index_info{.name = "uidx_users_email", .unique = true, .columns = {"email"}},
        },
    };

    const auto result = entityfy::validate_schema(model, table);

    REQUIRE_FALSE(result.valid());
    REQUIRE(result.issues.size() == 1);

    const auto& issue = result.issues.front();
    REQUIRE(issue.kind == entityfy::schema_issue_kind::primary_key_mismatch);
    REQUIRE(issue.object_name == "id");
    REQUIRE(issue.expected == "primary key");
    REQUIRE(issue.actual == "not primary key");
    REQUIRE(issue.message == "column primary-key status differs from the model");
}
