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
    const auto constraints = backend.query(
        "SELECT name, OBJECT_NAME(parent_object_id) FROM sys.foreign_keys "
        "WHERE schema_id = SCHEMA_ID()");

    for(const auto& row: constraints)
    {
        if(row.size() >= 2)
        {
            backend.execute_sql("ALTER TABLE \"" + row[1] + "\" DROP CONSTRAINT \"" + row[0] + "\"");
        }
    }

    const auto tables = backend.query("SELECT name FROM sys.tables WHERE schema_id = SCHEMA_ID()");
    for(const auto& row: tables)
    {
        if(!row.empty())
        {
            backend.execute_sql("DROP TABLE IF EXISTS \"" + row.front() + "\"");
        }
    }
}

} // namespace

entityfy_test::status test_sqlserver()
{
    const char* uri = std::getenv("ENTITYFY_SQLSERVER_URI");
    if(uri == nullptr || std::string{uri}.empty())
    {
        const char* require_sqlserver = std::getenv("ENTITYFY_REQUIRE_SQLSERVER");
        const bool  required = require_sqlserver != nullptr && std::string{require_sqlserver} == "1";

        if(required)
        {
            throw std::runtime_error{"ENTITYFY_REQUIRE_SQLSERVER=1 but ENTITYFY_SQLSERVER_URI is not set"};
        }

        std::cout << "sqlserver skipped: ENTITYFY_SQLSERVER_URI is not set\n";
        return entityfy_test::status::skipped;
    }

    auto backend = entityfy::migrate::open_sqlserver_provider(uri);
    cleanup(*backend);

    const auto root = test_workspace("entityfy_sqlserver_test_migrations");
    std::filesystem::remove_all(root);

    const entityfy::migrate::schema initial = current_schema(entityfy::dialect::sqlserver);

    const entityfy::migrate::schema v2 = [&] {
        auto value = initial;
        entityfy::migrate::find_table(value, "users")->columns.push_back(
            entityfy::migrate::column{
                .name = "nickname",
                .type = "NVARCHAR(40)",
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
        entityfy::migrate::find_column(*entityfy::migrate::find_table(value, "users"), "email")->type = "NVARCHAR(100)";
        return value;
    }();

    const entityfy::migrate::schema v5 = [&] {
        auto value = v4;
        entityfy::migrate::find_table(value, "users")->columns.push_back(
            entityfy::migrate::column{.name = "extra_note", .type = "NVARCHAR(30)"});
        return value;
    }();

    std::ostringstream output;

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "sqlserver",
                                        .allow_destructive = false,
                                        .backup = false,
                                    }};

        (void)runner.add(initial, "init");
        runner.apply(initial, output);
        require(entityfy::migrate::same_schema(initial, backend->introspect()), "sqlserver initial apply matches model");

        backend->execute_sql("INSERT INTO \"users\" (\"email\", \"name\") VALUES (N'ada@test.com', N'Ada')");
        backend->execute_sql(
            "INSERT INTO \"posts\" (\"user_id\", \"title\", \"body\", \"published\") VALUES (1, N'p', N'b', 1)");

        (void)runner.add(v2, "nickname");
        runner.apply(v2, output);
        require(entityfy::migrate::same_schema(v2, backend->introspect()), "sqlserver add column applied");

        auto rows = backend->query("SELECT \"nickname\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "yok", "sqlserver default applied to existing row");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "sqlserver",
                                        .allow_destructive = false,
                                        .backup = false,
                                        .renames = {{"users.name", "full_name"}},
                                    }};

        (void)runner.add(v3, "rename_name");
        runner.apply(v3, output);
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "sqlserver rename applied");

        auto rows = backend->query("SELECT \"full_name\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "Ada", "sqlserver data preserved after rename");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "sqlserver",
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

        require(refused, "sqlserver destructive migration refused by default");
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "sqlserver database unchanged after refusal");
    }

    std::filesystem::path engine_backup_path;

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "sqlserver",
                                        .allow_destructive = true,
                                        .backup = true,
                                    }};

        runner.apply(v4, output);
        require(entityfy::migrate::same_schema(v4, backend->introspect()), "sqlserver type change applied");

        for(const auto& entry: std::filesystem::directory_iterator(root))
        {
            const auto name = entry.path().filename().string();
            if(name.starts_with("backup-") && entry.path().extension() == ".sql")
            {
                engine_backup_path = entry.path();
            }
        }

        require(!engine_backup_path.empty(), "sqlserver engine writes a .sql backup before destructive steps");

        auto rows = backend->query("SELECT \"email\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "ada@test.com", "sqlserver data preserved after type change");

        (void)runner.add(v5, "extra_note");
        runner.apply(v5, output);
        require(entityfy::migrate::same_schema(v5, backend->introspect()), "sqlserver column added with data");

        runner.rollback_last(output);
        require(entityfy::migrate::same_schema(v4, backend->introspect()), "sqlserver rollback drops the column again");

        runner.status(v4, output);
        require(output.str().find("in sync") != std::string::npos, "sqlserver status reports in sync");
    }

    backend->execute_sql("DELETE FROM \"users\" WHERE \"id\" = 1");
    const auto remaining = backend->query("SELECT COUNT(*) FROM \"posts\"");
    require(remaining.size() == 1 && remaining.front().front() == "0", "sqlserver foreign key cascade works");

    backend->execute_sql("CREATE TABLE \"backup_probe\" (\"a\" INTEGER, \"b\" NVARCHAR(20))");
    backend->execute_sql("INSERT INTO \"backup_probe\" (\"a\", \"b\") VALUES (1, NULL)");
    backend->execute_sql("INSERT INTO \"backup_probe\" (\"a\", \"b\") VALUES (2, N'it''s')");

    require(backend->backup_extension() == ".sql", "sqlserver backup uses the sql extension");

    const auto dump_path = root / "manual-backup.sql";
    backend->backup(dump_path.string());
    require(std::filesystem::exists(dump_path), "sqlserver backup file written");

    const auto dump = read_text(dump_path);
    require(dump.find("CREATE TABLE \"backup_probe\"") != std::string::npos, "sqlserver backup includes the schema");
    require(
        dump.find("INSERT INTO \"backup_probe\" (\"a\", \"b\") VALUES (1, NULL);") != std::string::npos,
        "sqlserver backup preserves NULL values"
    );
    require(dump.find("VALUES (2, N'it''s');") != std::string::npos, "sqlserver backup escapes quotes");

    cleanup(*backend);
    restore_dump(*backend, dump);

    const auto restored = backend->query("SELECT \"a\", \"b\" FROM \"backup_probe\" ORDER BY \"a\"");
    require(restored.size() == 2, "sqlserver backup restores both rows");
    require(restored.front().size() == 2 && restored.front().front() == "1", "sqlserver restored integer column");
    require(restored.back().back() == "it's", "sqlserver restored text column");

    cleanup(*backend);
    restore_dump(*backend, read_text(engine_backup_path));

    const auto backup_users = backend->query("SELECT \"full_name\" FROM \"users\"");
    require(
        backup_users.size() == 1 && backup_users.front().front() == "Ada",
        "sqlserver engine backup restores the user row"
    );

    const auto backup_posts = backend->query("SELECT COUNT(*) FROM \"posts\"");
    require(
        backup_posts.size() == 1 && backup_posts.front().front() == "1",
        "sqlserver engine backup restores the dependent row"
    );

    cleanup(*backend);

    return entityfy_test::status::passed;
}
