#include "support/Fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("annotations produce model descriptors and DDL", "[schema]")
{
    const auto model = entityfy::describe_model<entityfy::test::User>(entityfy::dialect::sqlite);

    REQUIRE(model.table_name == "users");
    REQUIRE(model.columns.size() == 5);

    const auto& id = entityfy::primary_key_column(model);
    REQUIRE(id.name == "id");
    REQUIRE(id.flags.primary_key);
    REQUIRE(id.flags.auto_increment);
    REQUIRE(id.generated_on_insert);

    const auto& email = entityfy::test::column_by_name(model, "email");
    REQUIRE(email.flags.unique);
    REQUIRE(email.flags.indexed);
    REQUIRE(email.flags.not_null);
    REQUIRE(email.sql_type == "VARCHAR(320)");

    const auto statements = entityfy::create_schema_statements(model);
    REQUIRE(statements.size() >= 2);
    REQUIRE(statements.front().sql.find("CREATE TABLE IF NOT EXISTS \"users\"") != std::string::npos);

    bool found_unique_index = false;
    for(const auto& statement: statements)
    {
        found_unique_index = found_unique_index || statement.sql.find("CREATE UNIQUE INDEX IF NOT EXISTS") != std::string::npos;
    }

    REQUIRE(found_unique_index);
}

TEST_CASE("foreign key annotations produce relation descriptors", "[schema][relations]")
{
    const auto model = entityfy::describe_model<entityfy::test::Post>(entityfy::dialect::sqlite);

    REQUIRE(model.relations.size() == 1);
    REQUIRE(model.relations.front().local_column == "user_id");
    REQUIRE(model.relations.front().referenced_table == "users");
    REQUIRE(model.relations.front().referenced_column == "id");
    REQUIRE(model.relations.front().on_delete == "CASCADE");
}
