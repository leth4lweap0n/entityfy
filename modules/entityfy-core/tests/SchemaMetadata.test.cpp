#include "support/Fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace
{

struct [[= entityfy::table{"pg_systoken_model"}]] pg_systoken_model
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::not_null]]
    std::string name;

    [[= entityfy::xmin]]
    std::uint32_t revision = 0;
};

} // namespace

TEST_CASE("xmin maps the PostgreSQL system column", "[schema][postgresql]")
{
    const auto model = entityfy::describe_model<pg_systoken_model>(entityfy::dialect::postgresql);
    REQUIRE(model.columns.size() == 3);

    const auto revision_column = std::ranges::find_if(
        model.columns, [](const entityfy::column_descriptor& column) { return column.name == "xmin"; });
    REQUIRE(revision_column != model.columns.end());

    const auto& revision = *revision_column;
    REQUIRE(revision.flags.system);
    REQUIRE(revision.sql_type == "xid");
    REQUIRE_FALSE(revision.generated_on_insert);

    const auto create = entityfy::create_table(model);
    REQUIRE(create.sql.find("xmin") == std::string::npos);

    const auto select = entityfy::select_statement(model);
    REQUIRE(select.sql.find("\"xmin\"") != std::string::npos);

    const pg_systoken_model row{.id = 1, .name = "one", .revision = 0};

    const auto insert = entityfy::insert_statement(row, model);
    REQUIRE(insert.sql.find("xmin") == std::string::npos);
    REQUIRE(insert.binds.size() == 1);

    const auto many = entityfy::insert_many_statement(std::vector<pg_systoken_model>{row}, model);
    REQUIRE(many.sql.find("xmin") == std::string::npos);
    REQUIRE(many.binds.size() == 1);

    const auto update = entityfy::update_statement(row, model);
    REQUIRE(update.sql.find("xmin") == std::string::npos);

    REQUIRE(entityfy::create_index_statements(model).empty());
}

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
