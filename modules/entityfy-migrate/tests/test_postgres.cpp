#include "harness.hpp"

#include <entityfy/migrate/engine.hpp>
#include <entityfy/migrate/provider.hpp>

#include "models.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace
{

void cleanup(entityfy::migrate::provider& backend)
{
    backend.execute_sql("DROP TABLE IF EXISTS \"posts\" CASCADE");
    backend.execute_sql("DROP TABLE IF EXISTS \"users\" CASCADE");
    backend.execute_sql("DROP TABLE IF EXISTS \"entityfy_schema_migrations\" CASCADE");
}

std::string history_count(entityfy::migrate::provider& backend)
{
    const auto rows = backend.query("SELECT COUNT(*) FROM \"entityfy_schema_migrations\"");
    return rows.empty() || rows.front().empty() ? "0" : rows.front().front();
}

} // namespace

entityfy_test::status test_postgres()
{
    const char* uri = std::getenv("ENTITYFY_PG_URI");
    if(uri == nullptr || std::string{uri}.empty())
    {
        const char* require_pg = std::getenv("ENTITYFY_REQUIRE_PG");
        const bool required = require_pg != nullptr && std::string{require_pg} == "1";

        if(required)
        {
            throw std::runtime_error{"ENTITYFY_REQUIRE_PG=1 but ENTITYFY_PG_URI is not set"};
        }

        std::cout << "postgres skipped: ENTITYFY_PG_URI is not set\n";
        return entityfy_test::status::skipped;
    }

    auto backend = entityfy::migrate::open_postgres_provider(uri);
    cleanup(*backend);

    const auto root = test_workspace("entityfy_pg_test_migrations");
    std::filesystem::remove_all(root);

    const entityfy::migrate::schema initial = current_schema(entityfy::dialect::postgresql);

    const entityfy::migrate::schema v2 = [&] {
        auto value = initial;
        entityfy::migrate::find_table(value, "users")->columns.push_back(
            entityfy::migrate::column{.name = "nickname", .type = "VARCHAR(40)", .not_null = true,
                        .default_sql = "'yok'"});
        return value;
    }();

    const entityfy::migrate::schema v3 = [&] {
        auto value = v2;
        entityfy::migrate::find_column(*entityfy::migrate::find_table(value, "users"), "name")->name = "full_name";
        return value;
    }();

    const entityfy::migrate::schema v4 = [&] {
        auto value = v3;
        entityfy::migrate::find_column(*entityfy::migrate::find_table(value, "users"), "email")->type = "VARCHAR(100)";
        return value;
    }();

    const entityfy::migrate::schema v5 = [&] {
        auto value = v4;
        entityfy::migrate::find_table(value, "users")->columns.push_back(
            entityfy::migrate::column{.name = "required_code", .type = "VARCHAR(10)", .not_null = true});
        return value;
    }();

    std::ostringstream output;

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "postgres",
                                        .allow_destructive = false,
                                        .backup = false,
                                    }};

        (void)runner.add(initial, "init");
        runner.apply(initial, output);
        require(entityfy::migrate::same_schema(initial, backend->introspect()), "pg initial apply matches model");

        backend->execute_sql("INSERT INTO \"users\" (\"email\", \"name\") VALUES ('ada@test.com', 'Ada')");
        backend->execute_sql(
            "INSERT INTO \"posts\" (\"user_id\", \"title\", \"body\", \"published\") VALUES (1, 'p', 'b', true)");

        (void)runner.add(v2, "nickname");
        runner.apply(v2, output);
        require(entityfy::migrate::same_schema(v2, backend->introspect()), "pg add column applied");

        auto rows = backend->query("SELECT \"nickname\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "yok", "pg default applied to existing row");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "postgres",
                                        .allow_destructive = false,
                                        .backup = false,
                                        .renames = {{"users.name", "full_name"}},
                                    }};

        (void)runner.add(v3, "rename_name");
        runner.apply(v3, output);
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "pg rename applied");

        auto rows = backend->query("SELECT \"full_name\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "Ada", "pg data preserved after rename");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "postgres",
                                        .allow_destructive = false,
                                        .backup = false,
                                    }};

        (void)runner.add(v4, "email_shorter");

        bool refused = false;
        try
        {
            runner.apply(v4, output);
        }
        catch(const std::exception&)
        {
            refused = true;
        }

        require(refused, "pg destructive migration refused by default");
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "pg database unchanged after refusal");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "postgres",
                                        .allow_destructive = true,
                                        .backup = false,
                                    }};

        runner.apply(v4, output);
        require(entityfy::migrate::same_schema(v4, backend->introspect()), "pg type change applied");

        auto rows = backend->query("SELECT \"email\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "ada@test.com", "pg data preserved after type change");

        // NOT NULL column without a default: PostgreSQL errors, so the transaction must be rolled back.
        (void)runner.add(v5, "required_code");
        const std::string before = history_count(*backend);

        bool failed = false;
        try
        {
            runner.apply(v5, output);
        }
        catch(const std::exception&)
        {
            failed = true;
        }

        require(failed, "pg invalid NOT NULL migration failed");
        require(history_count(*backend) == before, "pg history rolled back");
        require(entityfy::migrate::same_schema(v4, backend->introspect()), "pg schema rolled back after failure");

        runner.rollback_last(output);
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "pg rollback restores previous schema");

        runner.status(v3, output);
        require(output.str().find("in sync") != std::string::npos, "pg status reports in sync");
    }

    backend->execute_sql("DELETE FROM \"users\" WHERE \"id\" = 1");
    const auto remaining = backend->query("SELECT COUNT(*) FROM \"posts\"");
    require(remaining.size() == 1 && remaining.front().front() == "0", "pg foreign key cascade works");

    cleanup(*backend);

    return entityfy_test::status::passed;
}
