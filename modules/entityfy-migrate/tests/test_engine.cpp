#include "harness.hpp"

#include <entityfy/migrate/engine.hpp>
#include <entityfy/migrate/json.hpp>
#include <entityfy/migrate/provider.hpp>

#include "models.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string_view>

namespace
{

std::vector<std::string> split_script(std::string_view text)
{
    std::vector<std::string> output;

    const auto sanitize = [](std::string_view input) {
        std::string filtered;
        std::istringstream stream{std::string{input}};
        std::string line;

        while(std::getline(stream, line))
        {
            const auto first = line.find_first_not_of(" \t\r");
            if(first != std::string::npos && (line[first] == '.' || line[first] == '\\'))
            {
                continue;
            }

            filtered += line;
            filtered.push_back('\n');
        }

        return filtered;
    };

    const auto push = [&](std::string_view input) {
        auto filtered = sanitize(input);
        if(filtered.find_first_not_of(" \t\r\n") != std::string::npos)
        {
            output.emplace_back(std::move(filtered));
        }
    };

    std::size_t start = 0;

    while(start < text.size())
    {
        const auto end = text.find(";\n", start);
        if(end == std::string_view::npos)
        {
            push(text.substr(start));
            break;
        }

        push(text.substr(start, end - start));
        start = end + 2;
    }

    return output;
}

} // namespace

entityfy_test::status test_engine()
{
    const auto database_path = test_workspace("entityfy_engine_test.db");
    const auto root = test_workspace("entityfy_engine_test_migrations");

    std::filesystem::remove(database_path);
    std::filesystem::remove_all(root);

    auto backend = entityfy::migrate::open_sqlite_provider(database_path.string());

    const entityfy::migrate::schema initial = current_schema(entityfy::dialect::sqlite);
    const entityfy::migrate::schema v2 = [&] {
        auto value = initial;
        entityfy::migrate::find_table(value, "users")->columns.push_back(
            entityfy::migrate::column{.name = "nickname", .type = "VARCHAR(40)", .not_null = true,
                        .default_sql = "'yok'"});
        return value;
    }();

    const entityfy::migrate::schema v3 = [&] {
        auto value = v2;
        auto* users = entityfy::migrate::find_table(value, "users");
        auto* name = entityfy::migrate::find_column(*users, "name");
        name->name = "full_name";
        return value;
    }();

    const entityfy::migrate::schema v4 = [&] {
        auto value = v3;
        entityfy::migrate::find_column(*entityfy::migrate::find_table(value, "users"), "email")->type = "VARCHAR(100)";
        return value;
    }();

    std::ostringstream output;

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "sqlite",
                                        .allow_destructive = false,
                                        .backup = false,
                                    }};

        const auto file = runner.add(initial, "init");
        require(!file.id.empty(), "init migration created");
        runner.apply(initial, output);
        require(entityfy::migrate::same_schema(initial, backend->introspect()), "initial apply matches model");

        backend->execute_sql("INSERT INTO \"users\" (\"email\", \"name\") VALUES ('ada@test.com', 'Ada')");
        backend->execute_sql(
            "INSERT INTO \"posts\" (\"user_id\", \"title\", \"body\", \"published\") VALUES (1, 'p', 'b', 1)");

        (void)runner.add(v2, "nickname");

        std::ostringstream script;
        runner.script_pending(v2, script);

        require(script.str().find("ALTER TABLE \"users\" ADD COLUMN \"nickname\"") != std::string::npos,
                "script contains alter statement");
        require(script.str().find("INSERT INTO \"entityfy_schema_migrations\"") != std::string::npos,
                "script records history");

        for(const auto& statement: split_script(script.str()))
        {
            backend->execute_sql(statement);
        }

        runner.apply(v2, output);
        require(entityfy::migrate::same_schema(v2, backend->introspect()), "add column applied");
        auto rows = backend->query("SELECT \"nickname\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "yok", "default applied to existing row");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "sqlite",
                                        .allow_destructive = false,
                                        .backup = false,
                                        .renames = {{"users.name", "full_name"}},
                                    }};

        (void)runner.add(v3, "rename_name");
        runner.apply(v3, output);
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "rename applied");
        auto rows = backend->query("SELECT \"full_name\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "Ada", "data preserved after rename");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "sqlite",
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

        require(refused, "destructive migration refused by default");
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "database unchanged after refusal");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "sqlite",
                                        .allow_destructive = true,
                                        .backup = false,
                                    }};

        runner.apply(v4, output);
        require(entityfy::migrate::same_schema(v4, backend->introspect()), "type change applied");
        auto rows = backend->query("SELECT \"email\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "ada@test.com", "data preserved after type change");

        runner.rollback_last(output);
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "rollback restores previous schema");

        runner.status(v3, output);
        require(output.str().find("in sync") != std::string::npos, "status reports in sync");
    }

    {
        const auto source_path = test_workspace("entityfy_script_source.db");
        const auto target_path = test_workspace("entityfy_script_target.db");
        const auto script_root = test_workspace("entityfy_script_migrations");

        std::filesystem::remove(source_path);
        std::filesystem::remove(target_path);
        std::filesystem::remove_all(script_root);

        auto source = entityfy::migrate::open_sqlite_provider(source_path.string());
        entityfy::migrate::engine source_runner{*source, entityfy::migrate::engine_options{
                                             .root = script_root,
                                             .provider_name = "sqlite",
                                             .allow_destructive = false,
                                             .backup = false,
                                         }};

        (void)source_runner.add(initial, "init");

        std::ostringstream script;
        source_runner.script_pending(initial, script);

        require(script.str().find("CREATE TABLE IF NOT EXISTS \"entityfy_schema_migrations\"") !=
                    std::string::npos,
                "script creates history table");

        const auto history_in_source = source->query(
            "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'entityfy_schema_migrations'");
        require(history_in_source.empty(), "script generation does not mutate source database");

        auto target = entityfy::migrate::open_sqlite_provider(target_path.string());
        for(const auto& statement: split_script(script.str()))
        {
            target->execute_sql(statement);
        }

        require(entityfy::migrate::same_schema(initial, target->introspect()), "script applies to a fresh target");
        const auto history = target->query("SELECT COUNT(*) FROM \"entityfy_schema_migrations\"");
        require(history.size() == 1 && history.front().front() == "1", "script records history on fresh target");

        source->close();
        target->close();

        std::filesystem::remove(source_path);
        std::filesystem::remove(target_path);
        std::filesystem::remove_all(script_root);
    }

    {
        const auto fail_path = test_workspace("entityfy_script_fail.db");
        const auto fail_root = test_workspace("entityfy_script_fail_migrations");

        std::filesystem::remove(fail_path);
        std::filesystem::remove_all(fail_root);

        auto source = entityfy::migrate::open_sqlite_provider(fail_path.string());
        entityfy::migrate::engine runner{*source, entityfy::migrate::engine_options{
                                        .root = fail_root,
                                        .provider_name = "sqlite",
                                        .allow_destructive = false,
                                        .backup = false,
                                    }};

        (void)runner.add(initial, "init");
        runner.apply(initial, output);

        source->execute_sql("INSERT INTO \"users\" (\"email\", \"name\") VALUES ('bad@test.com', 'Bad')");

        auto bad = initial;
        entityfy::migrate::find_table(bad, "users")->columns.push_back(
            entityfy::migrate::column{.name = "required_code", .type = "VARCHAR(10)", .not_null = true});

        const auto bad_file = runner.add(bad, "bad");

        std::ostringstream script;
        runner.script_pending(bad, script);
        require(script.str().find(".bail on") != std::string::npos, "script requests sqlite bail on error");

        bool failed = false;
        try
        {
            for(const auto& statement: split_script(script.str()))
            {
                source->execute_sql(statement);
            }
        }
        catch(const std::exception&)
        {
            failed = true;
        }

        require(failed, "failing script statement stops execution");

        try
        {
            source->execute_sql("ROLLBACK;");
        }
        catch(...)
        {
        }

        const auto history = source->query(
            "SELECT COUNT(*) FROM \"entityfy_schema_migrations\" WHERE \"id\" = " +
            entityfy::migrate::quote_literal(bad_file.id));
        require(history.size() == 1 && history.front().front() == "0", "failed script writes no history");

        source->close();
        std::filesystem::remove(fail_path);
        std::filesystem::remove_all(fail_root);
    }

    {
        const auto source_path = test_workspace("entityfy_rebuild_script_source.db");
        const auto target_path = test_workspace("entityfy_rebuild_script_target.db");
        const auto script_root = test_workspace("entityfy_rebuild_script_migrations");

        std::filesystem::remove(source_path);
        std::filesystem::remove(target_path);
        std::filesystem::remove_all(script_root);

        auto source = entityfy::migrate::open_sqlite_provider(source_path.string());
        entityfy::migrate::engine source_runner{*source, entityfy::migrate::engine_options{
                                             .root = script_root,
                                             .provider_name = "sqlite",
                                             .allow_destructive = false,
                                             .backup = false,
                                         }};

        (void)source_runner.add(initial, "init");
        source_runner.apply(initial, output);

        source->execute_sql("INSERT INTO \"users\" (\"email\", \"name\") VALUES ('u1@test.com', 'U1')");
        source->execute_sql("INSERT INTO \"users\" (\"email\", \"name\") VALUES ('u2@test.com', 'U2')");
        source->execute_sql("DELETE FROM \"users\" WHERE \"email\" = 'u2@test.com'");
        source->execute_sql(
            "CREATE TRIGGER \"trg_users\" AFTER INSERT ON \"users\" BEGIN SELECT 1; END");

        auto changed = initial;
        entityfy::migrate::find_column(*entityfy::migrate::find_table(changed, "users"), "email")->type = "VARCHAR(100)";
        const auto changed_file = source_runner.add(changed, "email_short");

        std::ostringstream script;
        source_runner.script_pending(changed, script);
        source->close();

        std::filesystem::copy_file(
            source_path, target_path, std::filesystem::copy_options::overwrite_existing);

        auto target = entityfy::migrate::open_sqlite_provider(target_path.string());
        for(const auto& statement: split_script(script.str()))
        {
            target->execute_sql(statement);
        }

        const auto columns = target->query("PRAGMA table_xinfo(\"users\")");
        bool type_changed = false;
        for(const auto& row: columns)
        {
            if(row.size() >= 3 && row[1] == "email")
            {
                type_changed = entityfy::migrate::same_type(row[2], "VARCHAR(100)");
            }
        }

        require(type_changed, "rebuild script changes column type");

        const auto rows = target->query("SELECT \"email\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "u1@test.com", "rebuild script preserves data");

        const auto sequence = target->query("SELECT seq FROM sqlite_sequence WHERE name = 'users'");
        require(sequence.size() == 1 && sequence.front().front() == "2",
                "rebuild script preserves autoincrement sequence");

        const auto triggers = target->query(
            "SELECT name FROM sqlite_master WHERE type = 'trigger' AND name = 'trg_users'");
        require(triggers.size() == 1, "rebuild script preserves triggers");

        const auto history = target->query(
            "SELECT COUNT(*) FROM \"entityfy_schema_migrations\" WHERE \"id\" = " +
            entityfy::migrate::quote_literal(changed_file.id));
        require(history.size() == 1 && history.front().front() == "1", "rebuild script records history");

        target->close();
        std::filesystem::remove(source_path);
        std::filesystem::remove(target_path);
        std::filesystem::remove_all(script_root);
    }

    {
        const auto source_path = test_workspace("entityfy_script_all_source.db");
        const auto target_path = test_workspace("entityfy_script_all_target.db");
        const auto script_root = test_workspace("entityfy_script_all_migrations");

        std::filesystem::remove(source_path);
        std::filesystem::remove(target_path);
        std::filesystem::remove_all(script_root);

        auto source = entityfy::migrate::open_sqlite_provider(source_path.string());
        entityfy::migrate::engine runner{*source, entityfy::migrate::engine_options{
                                        .root = script_root,
                                        .provider_name = "sqlite",
                                        .allow_destructive = false,
                                        .backup = false,
                                    }};

        (void)runner.add(initial, "init");
        (void)runner.add(v2, "nickname");

        auto changed = v2;
        entityfy::migrate::find_column(*entityfy::migrate::find_table(changed, "users"), "email")->type = "VARCHAR(100)";
        (void)runner.add(changed, "email_short");

        std::ostringstream script;
        runner.script_all(script);

        require(script.str().find("-- migration") != std::string::npos, "all script has migrations");

        auto target = entityfy::migrate::open_sqlite_provider(target_path.string());
        for(const auto& statement: split_script(script.str()))
        {
            target->execute_sql(statement);
        }

        require(entityfy::migrate::same_schema(changed, target->introspect()), "all script builds the final schema");

        const auto history = target->query("SELECT COUNT(*) FROM \"entityfy_schema_migrations\"");
        require(history.size() == 1 && history.front().front() == "3", "all script records every migration");

        target->close();
        source->close();
        std::filesystem::remove(source_path);
        std::filesystem::remove(target_path);
        std::filesystem::remove_all(script_root);
    }

    {
        const auto source_path = test_workspace("entityfy_script_rewrite_source.db");
        const auto target_path = test_workspace("entityfy_script_rewrite_target.db");
        const auto script_root = test_workspace("entityfy_script_rewrite_migrations");

        std::filesystem::remove(source_path);
        std::filesystem::remove(target_path);
        std::filesystem::remove_all(script_root);

        entityfy::migrate::schema v1;
        entityfy::migrate::table legacy_v1;
        legacy_v1.name = "legacy";
        legacy_v1.columns.push_back(entityfy::migrate::column{.name = "old_id", .type = "INTEGER", .not_null = true, .primary_key = true,
                                                .auto_increment = true});
        legacy_v1.columns.push_back(entityfy::migrate::column{.name = "old_value", .type = "VARCHAR(10)"});
        v1.tables.push_back(legacy_v1);

        entityfy::migrate::schema v2;
        entityfy::migrate::table legacy_v2;
        legacy_v2.name = "legacy";
        legacy_v2.columns.push_back(entityfy::migrate::column{.name = "new_id", .type = "INTEGER", .not_null = true, .primary_key = true,
                                                .auto_increment = true});
        legacy_v2.columns.push_back(entityfy::migrate::column{.name = "new_value", .type = "VARCHAR(10)"});
        v2.tables.push_back(legacy_v2);

        auto source = entityfy::migrate::open_sqlite_provider(source_path.string());
        entityfy::migrate::engine runner{*source, entityfy::migrate::engine_options{
                                        .root = script_root,
                                        .provider_name = "sqlite",
                                        .allow_destructive = false,
                                        .backup = false,
                                    }};

        (void)runner.add(v1, "legacy_init");
        (void)runner.add(v2, "legacy_rewrite");

        std::ostringstream script;
        runner.script_all(script);

        require(script.str().find("__entityfy_new_legacy") != std::string::npos,
                "no-common rebuild is rendered on empty source");

        auto target = entityfy::migrate::open_sqlite_provider(target_path.string());
        for(const auto& statement: split_script(script.str()))
        {
            target->execute_sql(statement);
        }

        if(!entityfy::migrate::same_schema(v2, target->introspect()))
        {
            std::cout << "expected:\n" << entityfy::migrate::schema_to_json(v2) << "\nactual:\n"
                      << entityfy::migrate::schema_to_json(target->introspect()) << "\n";
        }
        require(entityfy::migrate::same_schema(v2, target->introspect()), "no-common rebuild script applies to fresh target");

        const auto history = target->query("SELECT COUNT(*) FROM \"entityfy_schema_migrations\"");
        require(history.size() == 1 && history.front().front() == "2", "no-common script records both migrations");

        target->close();
        source->close();
        std::filesystem::remove(source_path);
        std::filesystem::remove(target_path);
        std::filesystem::remove_all(script_root);
    }

    {
        const auto key_path = test_workspace("entityfy_key_change_test.db");
        std::filesystem::remove(key_path);

        auto backend = entityfy::migrate::open_sqlite_provider(key_path.string());

        const auto apply = [&](const entityfy::migrate::plan& value) {
            backend->start();
            try
            {
                for(const auto& current: value.steps)
                {
                    backend->execute(current);
                }

                backend->finish();
            }
            catch(...)
            {
                backend->abort();
                throw;
            }
        };

        entityfy::migrate::schema single;

        entityfy::migrate::table lines;
        lines.name = "key_lines";
        lines.primary_key = {"id"};
        lines.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true,
                                                          .primary_key = true});
        lines.columns.push_back(entityfy::migrate::column{.name = "region", .type = "VARCHAR(16)", .not_null = true});
        lines.columns.push_back(entityfy::migrate::column{.name = "quantity", .type = "INTEGER", .not_null = true});
        single.tables.push_back(lines);

        const auto creation = backend->make_plan(single, entityfy::migrate::schema{}, {});
        require(creation.steps.size() == 1, "sqlite key change setup creates the table");
        apply(creation);

        backend->execute_sql("INSERT INTO \"key_lines\" (\"id\", \"region\", \"quantity\") VALUES (1, 'eu', 3)");
        backend->execute_sql("INSERT INTO \"key_lines\" (\"id\", \"region\", \"quantity\") VALUES (2, 'us', 4)");

        const auto before = backend->introspect();
        const auto* before_lines = entityfy::migrate::find_table(before, "key_lines");
        require(before_lines != nullptr &&
                    entityfy::migrate::primary_key_columns(*before_lines) == std::vector<std::string>{"id"},
                "sqlite key change starts from a single-column key");

        auto promoted = single;
        auto* promoted_lines = entityfy::migrate::find_table(promoted, "key_lines");
        promoted_lines->primary_key = {"id", "region"};
        entityfy::migrate::find_column(*promoted_lines, "region")->primary_key = true;

        const auto change = backend->make_plan(promoted, before, {});
        require(change.destructive, "sqlite primary key change is destructive");
        require(!change.steps.empty() && change.steps.front().type == entityfy::migrate::step::kind::sqlite_rebuild,
                "sqlite primary key change rebuilds the table");
        apply(change);

        const auto after = backend->introspect();
        const auto* after_lines = entityfy::migrate::find_table(after, "key_lines");
        require(after_lines != nullptr &&
                    entityfy::migrate::primary_key_columns(*after_lines) == std::vector<std::string>{"id", "region"},
                "sqlite promoted key introspected in order");
        require(entityfy::migrate::same_schema(promoted, after), "sqlite promoted key matches the model");

        const auto rows = backend->query("SELECT \"id\", \"region\", \"quantity\" FROM \"key_lines\" ORDER BY \"id\"");
        require(rows.size() == 2 && rows.front().size() == 3 && rows.front()[1] == "eu" &&
                    rows.back()[1] == "us" && rows.back()[2] == "4",
                "sqlite rows survive the primary key change");

        bool duplicate_rejected = false;
        try
        {
            backend->execute_sql("INSERT INTO \"key_lines\" (\"id\", \"region\", \"quantity\") VALUES (1, 'eu', 9)");
        }
        catch(const std::exception&)
        {
            duplicate_rejected = true;
        }

        require(duplicate_rejected, "sqlite promoted key is enforced");

        backend->close();
        std::filesystem::remove(key_path);
    }

    return entityfy_test::status::passed;
}

entityfy_test::status test_history_table()
{
    const auto database_path = test_workspace("entityfy_history_test.db");
    const auto root = test_workspace("entityfy_history_migrations");

    std::filesystem::remove(database_path);
    std::filesystem::remove_all(root);

    auto backend = entityfy::migrate::open_sqlite_provider(database_path.string());

    entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                                   .root = root,
                                                   .provider_name = "sqlite",
                                                   .allow_destructive = false,
                                                   .backup = false,
                                               }};

    const auto applied = runner.applied_ids();
    require(applied.empty(), "a fresh database has no applied migrations");

    const auto tables = backend->query("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name");

    require(tables.size() == 1, "engine creates only the history table");
    require(!tables.front().empty() && tables.front().front() == "entityfy_schema_migrations",
            "history table uses the entityfy name");

    backend->close();
    std::filesystem::remove(database_path);
    std::filesystem::remove_all(root);

    return entityfy_test::status::passed;
}
