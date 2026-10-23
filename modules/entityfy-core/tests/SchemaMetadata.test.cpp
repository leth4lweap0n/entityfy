#include "support/Fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

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

namespace
{

struct [[= entityfy::table{"key_regions"}]] composite_region
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::primary_key, = entityfy::varchar{32}]]
    std::string region;

    [[= entityfy::not_null, = entityfy::varchar{200}]]
    std::string title;

    [[= entityfy::not_null]]
    std::int64_t score = 0;
};

// The text key part is declared first on purpose: descriptor order must follow
// the declaration, not the column names.
struct [[= entityfy::table{"key_region_reversed"}]] reversed_region
{
    [[= entityfy::primary_key, = entityfy::varchar{16}]]
    std::string region;

    [[= entityfy::primary_key]]
    std::int64_t id = 0;
};

struct [[= entityfy::table{"key_tags"}]] key_tag
{
    [[= entityfy::primary_key, = entityfy::varchar{64}]]
    std::string slug;

    [[= entityfy::not_null]]
    std::string label;
};

struct [[= entityfy::table{"key_people"}]] key_person
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;
};

} // namespace

TEST_CASE("composite primary keys keep declaration order in the descriptor", "[schema][primary_key]")
{
    const auto model = entityfy::describe_model<composite_region>(entityfy::dialect::sqlite);

    REQUIRE(model.primary_key_indices.size() == 2);
    REQUIRE(model.primary_key_indices[0] == 0);
    REQUIRE(model.primary_key_indices[1] == 1);

    const auto key_columns = entityfy::primary_key_columns(model);
    REQUIRE(key_columns.size() == 2);
    REQUIRE(key_columns[0].name == "id");
    REQUIRE(key_columns[1].name == "region");
    REQUIRE(key_columns[0].flags.primary_key);
    REQUIRE(key_columns[1].flags.primary_key);
    REQUIRE_FALSE(key_columns[0].flags.auto_increment);
    REQUIRE_FALSE(key_columns[1].flags.auto_increment);
    REQUIRE(key_columns[0].flags.not_null);
    REQUIRE(key_columns[1].flags.not_null);

    // There is no single key column to return for a composite key.
    REQUIRE_THROWS_AS(entityfy::primary_key_column(model), std::logic_error);
    REQUIRE_THROWS_AS(entityfy::primary_key_column<composite_region>(entityfy::dialect::sqlite), std::logic_error);
}

TEST_CASE("composite key order follows the declaration, not the column names", "[schema][primary_key]")
{
    const auto model = entityfy::describe_model<reversed_region>(entityfy::dialect::sqlite);

    REQUIRE(model.primary_key_indices.size() == 2);
    REQUIRE(model.primary_key_indices[0] == 0);
    REQUIRE(model.primary_key_indices[1] == 1);

    const auto key_columns = entityfy::primary_key_columns(model);
    REQUIRE(key_columns.size() == 2);
    REQUIRE(key_columns[0].name == "region");
    REQUIRE(key_columns[1].name == "id");

    REQUIRE(entityfy::create_table(model).sql.find("PRIMARY KEY (\"region\", \"id\")") != std::string::npos);
    REQUIRE_THROWS_AS(entityfy::primary_key_column(model), std::logic_error);
}

TEST_CASE("composite primary keys emit one table-level constraint", "[schema][primary_key]")
{
    const auto model = entityfy::describe_model<composite_region>(entityfy::dialect::sqlite);
    const auto create = entityfy::create_table(model);

    REQUIRE(create.sql ==
            "CREATE TABLE IF NOT EXISTS \"key_regions\" (\"id\" INTEGER NOT NULL, \"region\" VARCHAR(32) NOT NULL, "
            "\"title\" VARCHAR(200) NOT NULL, \"score\" INTEGER NOT NULL, PRIMARY KEY (\"id\", \"region\"))");

    // No column keeps the column-level constraint, and both key parts are NOT NULL.
    REQUIRE(create.sql.find("\"id\" INTEGER PRIMARY KEY") == std::string::npos);
    REQUIRE(create.sql.find("\"region\" VARCHAR(32) PRIMARY KEY") == std::string::npos);
    REQUIRE(create.sql.find("\"id\" INTEGER NOT NULL") != std::string::npos);
    REQUIRE(create.sql.find("\"region\" VARCHAR(32) NOT NULL") != std::string::npos);

    // The primary key already indexes its columns, so no extra index is created.
    REQUIRE(entityfy::create_index_statements(model).empty());

    const auto statements = entityfy::create_schema_statements(model);
    REQUIRE(statements.size() == 1);
    REQUIRE(statements.front().sql.find("INDEX") == std::string::npos);
}

// [[= entityfy::auto_increment]] together with a composite key is rejected by
// validate_model()'s static_assert, so that combination cannot be a runtime test.

TEST_CASE("a string primary key stays a single-column constraint", "[schema][primary_key]")
{
    const auto model = entityfy::describe_model<key_tag>(entityfy::dialect::sqlite);

    REQUIRE(model.primary_key_indices.size() == 1);
    REQUIRE(model.primary_key_indices.front() == 0);

    const auto key_column = entityfy::primary_key_column(model);
    REQUIRE(key_column.name == "slug");
    REQUIRE(key_column.flags.primary_key);
    REQUIRE_FALSE(key_column.flags.auto_increment);

    REQUIRE(entityfy::create_table(model).sql ==
            "CREATE TABLE IF NOT EXISTS \"key_tags\" (\"slug\" VARCHAR(64) PRIMARY KEY, \"label\" TEXT NOT NULL)");

    const auto insert = entityfy::insert_statement(key_tag{.slug = "alfa", .label = "Alfa"}, model);
    REQUIRE(insert.sql == "INSERT INTO \"key_tags\" (\"slug\", \"label\") VALUES (?, ?)");
    REQUIRE(insert.binds.size() == 2);
    REQUIRE(std::get<std::string>(insert.binds[0]) == "alfa");
    REQUIRE(std::get<std::string>(insert.binds[1]) == "Alfa");
}

TEST_CASE("an auto-increment primary key keeps the single-column DDL", "[schema][primary_key]")
{
    const auto model = entityfy::describe_model<key_person>(entityfy::dialect::sqlite);

    REQUIRE(model.primary_key_indices.size() == 1);
    REQUIRE(model.primary_key_indices.front() == 0);
    REQUIRE(entityfy::primary_key_column(model).name == "id");

    const auto create = entityfy::create_table(model);
    REQUIRE(create.sql ==
            "CREATE TABLE IF NOT EXISTS \"key_people\" (\"id\" INTEGER PRIMARY KEY AUTOINCREMENT, "
            "\"name\" VARCHAR(120) NOT NULL)");

    // The SQLite column-level form is only reachable with a single key part.
    REQUIRE(create.sql.find("INTEGER PRIMARY KEY AUTOINCREMENT") != std::string::npos);
    REQUIRE(create.sql.find("PRIMARY KEY (") == std::string::npos);
}
