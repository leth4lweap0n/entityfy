#include "harness.hpp"

#include <entityfy/migrate/engine.hpp>
#include <entityfy/migrate/provider.hpp>

#include "models.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace
{

void cleanup(entityfy::migrate::provider& backend)
{
    // The database is expected to be empty before the initial apply, so every
    // leftover table (from another test run included) is dropped.
    backend.execute_sql("SET FOREIGN_KEY_CHECKS = 0");
    const auto tables = backend.query(
        "SELECT table_name FROM information_schema.tables WHERE table_schema = DATABASE()");

    for(const auto& row: tables)
    {
        if(!row.empty())
        {
            backend.execute_sql("DROP TABLE IF EXISTS \"" + row.front() + "\"");
        }
    }

    backend.execute_sql("SET FOREIGN_KEY_CHECKS = 1");
}

} // namespace

entityfy_test::status test_mysql()
{
    const char* uri = std::getenv("ENTITYFY_MYSQL_URI");
    if(uri == nullptr || std::string{uri}.empty())
    {
        const char* require_mysql = std::getenv("ENTITYFY_REQUIRE_MYSQL");
        const bool        required = require_mysql != nullptr && std::string{require_mysql} == "1";

        if(required)
        {
            throw std::runtime_error{"ENTITYFY_REQUIRE_MYSQL=1 but ENTITYFY_MYSQL_URI is not set"};
        }

        std::cout << "mysql skipped: ENTITYFY_MYSQL_URI is not set\n";
        return entityfy_test::status::skipped;
    }

    auto backend = entityfy::migrate::open_mysql_provider(uri);
    cleanup(*backend);

    const auto root = test_workspace("entityfy_mysql_test_migrations");
    std::filesystem::remove_all(root);

    const entityfy::migrate::schema initial = current_schema(entityfy::dialect::mysql);

    const entityfy::migrate::schema v2 = [&] {
        auto value = initial;
        entityfy::migrate::find_table(value, "users")->columns.push_back(
            entityfy::migrate::column{
                .name = "nickname",
                .type = "VARCHAR(40)",
                .not_null = true,
                .default_sql = "'yok'",
            });
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
            entityfy::migrate::column{.name = "extra_note", .type = "VARCHAR(30)"});
        return value;
    }();

    std::ostringstream output;

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "mysql",
                                        .allow_destructive = false,
                                        .backup = false,
                                    }};

        (void)runner.add(initial, "init");
        runner.apply(initial, output);
        require(entityfy::migrate::same_schema(initial, backend->introspect()), "mysql initial apply matches model");

        backend->execute_sql("INSERT INTO \"users\" (\"email\", \"name\") VALUES ('ada@test.com', 'Ada')");
        backend->execute_sql(
            "INSERT INTO \"posts\" (\"user_id\", \"title\", \"body\", \"published\") VALUES (1, 'p', 'b', true)");

        (void)runner.add(v2, "nickname");
        runner.apply(v2, output);
        require(entityfy::migrate::same_schema(v2, backend->introspect()), "mysql add column applied");

        auto rows = backend->query("SELECT \"nickname\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "yok", "mysql default applied to existing row");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "mysql",
                                        .allow_destructive = false,
                                        .backup = false,
                                        .renames = {{"users.name", "full_name"}},
                                    }};

        (void)runner.add(v3, "rename_name");
        runner.apply(v3, output);
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "mysql rename applied");

        auto rows = backend->query("SELECT \"full_name\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "Ada", "mysql data preserved after rename");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "mysql",
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

        require(refused, "mysql destructive migration refused by default");
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "mysql database unchanged after refusal");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "mysql",
                                        .allow_destructive = true,
                                        .backup = true,
                                    }};

        runner.apply(v4, output);
        require(entityfy::migrate::same_schema(v4, backend->introspect()), "mysql type change applied");

        bool engine_backup = false;
        for(const auto& entry: std::filesystem::directory_iterator(root))
        {
            const auto name = entry.path().filename().string();
            if(name.starts_with("backup-") && entry.path().extension() == ".sql")
            {
                engine_backup = true;
            }
        }

        require(engine_backup, "mysql engine writes a .sql backup before destructive steps");

        auto rows = backend->query("SELECT \"email\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "ada@test.com", "mysql data preserved after type change");

        (void)runner.add(v5, "extra_note");
        runner.apply(v5, output);
        require(entityfy::migrate::same_schema(v5, backend->introspect()), "mysql column added with data");

        runner.rollback_last(output);
        require(entityfy::migrate::same_schema(v4, backend->introspect()), "mysql rollback drops the column again");

        runner.status(v4, output);
        require(output.str().find("in sync") != std::string::npos, "mysql status reports in sync");
    }

    backend->execute_sql("DELETE FROM \"users\" WHERE \"id\" = 1");
    const auto remaining = backend->query("SELECT COUNT(*) FROM \"posts\"");
    require(remaining.size() == 1 && remaining.front().front() == "0", "mysql foreign key cascade works");

    backend->execute_sql("CREATE TABLE \"backup_probe\" (\"a\" INTEGER, \"b\" TEXT)");
    backend->execute_sql("INSERT INTO \"backup_probe\" (\"a\", \"b\") VALUES (1, NULL)");
    backend->execute_sql("INSERT INTO \"backup_probe\" (\"a\", \"b\") VALUES (2, 'it''s')");

    require(backend->backup_extension() == ".sql", "mysql backup uses the sql extension");

    const auto dump_path = root / "manual-backup.sql";
    backend->backup(dump_path.string());
    require(std::filesystem::exists(dump_path), "mysql backup file written");

    const auto dump = read_text(dump_path);
    require(dump.find("CREATE TABLE \"backup_probe\"") != std::string::npos, "mysql backup includes the schema");
    require(
        dump.find("INSERT INTO \"backup_probe\" (\"a\", \"b\") VALUES (1, NULL);") != std::string::npos,
        "mysql backup preserves NULL values"
    );
    require(dump.find("VALUES (2, 'it''s');") != std::string::npos, "mysql backup escapes quotes");

    cleanup(*backend);
    restore_dump(*backend, dump);

    const auto restored = backend->query("SELECT \"a\", \"b\" FROM \"backup_probe\" ORDER BY \"a\"");
    require(restored.size() == 2, "mysql backup restores both rows");
    require(restored.front().size() == 2 && restored.front().front() == "1", "mysql restored integer column");
    require(restored.back().back() == "it's", "mysql restored text column");

    cleanup(*backend);

    return entityfy_test::status::passed;
}
