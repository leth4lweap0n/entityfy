#include "support/Fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

TEST_CASE("migration planning creates schema for missing tables", "[migration]")
{
    const auto model = entityfy::describe_model<entityfy::test::User>(entityfy::dialect::sqlite);
    const auto plan = entityfy::plan_migration(model, {});

    REQUIRE(!plan.empty());
    REQUIRE(plan.statements.front().sql.find("CREATE TABLE IF NOT EXISTS \"users\"") != std::string::npos);
}

TEST_CASE("migration planning adds missing columns for existing tables", "[migration]")
{
    const auto model = entityfy::describe_model<entityfy::test::User>(entityfy::dialect::sqlite);
    const auto plan = entityfy::plan_migration(model, std::vector<std::string>{"id", "email"});

    bool found_name_column = false;
    for(const auto& statement: plan.statements)
    {
        found_name_column = found_name_column || statement.sql.find("ADD COLUMN \"name\"") != std::string::npos;
    }

    REQUIRE(found_name_column);
}

TEST_CASE("versioned migration metadata has stable table contract", "[migration]")
{
    const auto create = entityfy::create_migration_table_statement();
    REQUIRE(create.sql.find("\"entityfy_schema_migrations\"") != std::string::npos);
    REQUIRE(create.sql.find("\"id\" TEXT PRIMARY KEY") != std::string::npos);

    const auto record = entityfy::record_migration_statement("001_initial");
    REQUIRE(record.sql.find("INSERT INTO \"entityfy_schema_migrations\"") != std::string::npos);
    REQUIRE(record.binds.size() == 1);
}

TEST_CASE("versioned migrations are recorded in the entityfy history table", "[migration]")
{
    entityfy::client db{"sqlite://:memory:"};

    db.apply_migrations({entityfy::migration{
        .id = "001_initial",
        .statements = {entityfy::statement{.sql = "CREATE TABLE IF NOT EXISTS \"probe\" (\"id\" INTEGER)"}},
    }});

    const auto recorded = db.query(entityfy::statement{
        .sql = "SELECT COUNT(*) FROM \"entityfy_schema_migrations\" WHERE \"id\" = '001_initial'"});

    REQUIRE(recorded.size() == 1);
    REQUIRE(std::get<std::int64_t>(recorded.front().front()) == 1);

    const auto tables = db.query(entityfy::statement{
        .sql = "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name"});

    bool history_found = false;
    for(const auto& row: tables)
    {
        history_found = history_found ||
                        (!row.empty() && std::get<std::string>(row.front()) == "entityfy_schema_migrations");
    }

    REQUIRE(history_found);
}

TEST_CASE("migrations are transactional by default", "[migration]")
{
    const entityfy::migration migration{
        .id = "001_initial",
        .statements = {},
    };

    REQUIRE(migration.transactional);
}
