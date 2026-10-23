#include "harness.hpp"

#include <entityfy/migrate/provider.hpp>
#include <entityfy/migrate/json.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{

void apply_plan(entityfy::migrate::provider& backend, const entityfy::migrate::plan& value)
{
    backend.start();
    try
    {
        for(const auto& current: value.steps)
        {
            backend.execute(current);
        }

        backend.finish();
    }
    catch(...)
    {
        backend.abort();
        throw;
    }
}

entityfy::migrate::schema base_schema()
{
    entityfy::migrate::schema output;

    entityfy::migrate::table users;
    users.name = "users";
    users.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true, .primary_key = true,
                                        .auto_increment = true});
    users.columns.push_back(entityfy::migrate::column{.name = "email", .type = "VARCHAR(320)", .not_null = true,
                                        .unique = true, .indexed = true});
    users.columns.push_back(entityfy::migrate::column{.name = "name", .type = "VARCHAR(120)", .not_null = true});
    users.indexes.push_back(entityfy::migrate::index{.name = "uidx_users_email", .columns = {"email"}, .unique = true});
    output.tables.push_back(users);

    entityfy::migrate::table posts;
    posts.name = "posts";
    posts.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true, .primary_key = true,
                                        .auto_increment = true});
    posts.columns.push_back(entityfy::migrate::column{.name = "user_id", .type = "INTEGER", .not_null = true, .indexed = true});
    posts.columns.back().references = entityfy::migrate::reference{.table = "users", .column = "id", .on_delete = "CASCADE"};
    posts.columns.push_back(entityfy::migrate::column{.name = "title", .type = "VARCHAR(200)", .not_null = true});
    posts.indexes.push_back(entityfy::migrate::index{.name = "idx_posts_user_id", .columns = {"user_id"}, .unique = false});
    posts.foreign_keys.push_back(
        entityfy::migrate::foreign_key{.column = "user_id", .ref_table = "users", .ref_column = "id", .on_delete = "CASCADE"});
    output.tables.push_back(posts);

    return output;
}

} // namespace

entityfy_test::status test_sqlite()
{
    const auto path = test_workspace("entityfy_sqlite_test.db");
    std::filesystem::remove(path);

    auto backend = entityfy::migrate::open_sqlite_provider(path.string());
    const entityfy::migrate::schema desired = base_schema();

    const auto creation = backend->make_plan(desired, entityfy::migrate::schema{}, {});
    require(!creation.steps.empty(), "creation plan is not empty");
    require(!creation.destructive, "creation is not destructive");
    apply_plan(*backend, creation);

    auto actual = backend->introspect();
    if(!entityfy::migrate::same_schema(desired, actual))
    {
        std::cout << "desired:\n" << entityfy::migrate::schema_to_json(desired) << "\nactual:\n"
                  << entityfy::migrate::schema_to_json(actual) << "\n";
    }
    require(entityfy::migrate::same_schema(desired, actual), "create table roundtrip");

    backend->execute_sql("INSERT INTO \"users\" (\"email\", \"name\") VALUES ('ada@test.com', 'Ada')");
    backend->execute_sql("INSERT INTO \"posts\" (\"user_id\", \"title\") VALUES (1, 'first')");

    // adding a nullable column must use ALTER TABLE (no rebuild needed)
    entityfy::migrate::schema with_nickname = desired;
    entityfy::migrate::find_table(with_nickname, "users")->columns.push_back(
        entityfy::migrate::column{.name = "nickname", .type = "VARCHAR(40)"});

    const auto add_plan = backend->make_plan(with_nickname, actual, {});
    require(add_plan.steps.size() == 1, "add column single step");
    apply_plan(*backend, add_plan);

    actual = backend->introspect();
    require(entityfy::migrate::same_schema(with_nickname, actual), "add column roundtrip");

    backend->execute_sql(
        "CREATE TRIGGER \"trg_users\" AFTER INSERT ON \"users\" BEGIN SELECT 1; END");

    // type change -> rebuild, data must be preserved, trigger must be preserved
    entityfy::migrate::schema tightened = with_nickname;
    entityfy::migrate::find_column(*entityfy::migrate::find_table(tightened, "users"), "email")->type = "VARCHAR(100)";

    const auto rebuild_plan = backend->make_plan(tightened, actual, {});
    require(rebuild_plan.destructive, "type change destructive");
    require(!rebuild_plan.steps.empty(), "rebuild plan not empty");

    const auto rebuild_step = std::ranges::find_if(rebuild_plan.steps, [](const entityfy::migrate::step& value) {
        return value.type == entityfy::migrate::step::kind::sqlite_rebuild;
    });
    require(rebuild_step != rebuild_plan.steps.end(), "rebuild step present");

    const auto rebuild_sql = backend->render_rebuild(rebuild_step->desired_table, rebuild_step->actual_table);
    require(std::ranges::any_of(rebuild_sql, [](const std::string& sql) {
                return sql.find("__entityfy_new_users") != std::string::npos;
            }),
            "rebuild script creates temporary table");
    require(std::ranges::any_of(rebuild_sql, [](const std::string& sql) {
                return sql.find("RENAME TO") != std::string::npos;
            }),
            "rebuild script renames temporary table");

    apply_plan(*backend, rebuild_plan);

    actual = backend->introspect();
    require(entityfy::migrate::same_schema(tightened, actual), "type change roundtrip");

    auto rows = backend->query("SELECT \"email\", \"name\" FROM \"users\"");
    require(rows.size() == 1 && rows.front()[0] == "ada@test.com" && rows.front()[1] == "Ada",
            "row preserved after rebuild");

    auto triggers = backend->query("SELECT \"name\" FROM sqlite_master WHERE type = 'trigger'");
    require(triggers.size() == 1 && triggers.front().front() == "trg_users", "trigger preserved after rebuild");

    // dropping a column -> rebuild
    entityfy::migrate::schema without_nickname = tightened;
    auto* users = entityfy::migrate::find_table(without_nickname, "users");
    users->columns.erase(std::ranges::find_if(users->columns, [](const entityfy::migrate::column& value) {
        return value.name == "nickname";
    }));

    const auto drop_plan = backend->make_plan(without_nickname, actual, {});
    require(drop_plan.destructive, "drop column destructive");
    apply_plan(*backend, drop_plan);

    actual = backend->introspect();
    require(entityfy::migrate::same_schema(without_nickname, actual), "drop column roundtrip");

    rows = backend->query("SELECT \"email\" FROM \"users\"");
    require(rows.size() == 1 && rows.front()[0] == "ada@test.com", "row preserved after drop column");

    // is the FK still working?
    backend->execute_sql("DELETE FROM \"users\" WHERE \"id\" = 1");
    rows = backend->query("SELECT COUNT(*) FROM \"posts\"");
    require(rows.size() == 1 && rows.front()[0] == "0", "foreign key cascade still works");

    // Composite keys use a table-level PRIMARY KEY and must round-trip; a
    // single-column key keeps its column-level form.
    {
        const auto composite_path = test_workspace("entityfy_sqlite_composite_test.db");
        std::filesystem::remove(composite_path);

        auto composite_backend = entityfy::migrate::open_sqlite_provider(composite_path.string());

        entityfy::migrate::schema composite;

        entityfy::migrate::table lines;
        lines.name = "key_lines";
        lines.primary_key = {"id", "region"};
        lines.columns.push_back(
            entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true, .primary_key = true});
        lines.columns.push_back(
            entityfy::migrate::column{.name = "region", .type = "TEXT", .not_null = true, .primary_key = true});
        lines.columns.push_back(entityfy::migrate::column{.name = "quantity", .type = "INTEGER", .not_null = true});
        composite.tables.push_back(lines);

        entityfy::migrate::table notes;
        notes.name = "key_notes";
        notes.primary_key = {"id"};
        notes.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true,
                                            .primary_key = true, .auto_increment = true});
        notes.columns.push_back(entityfy::migrate::column{.name = "body", .type = "TEXT", .not_null = true});
        composite.tables.push_back(notes);

        const auto creation = composite_backend->make_plan(composite, entityfy::migrate::schema{}, {});
        require(creation.steps.size() == 2, "composite creation creates both tables");
        require(!creation.destructive, "composite creation is not destructive");
        apply_plan(*composite_backend, creation);

        auto actual = composite_backend->introspect();
        if(!entityfy::migrate::same_schema(composite, actual))
        {
            std::cout << "desired:\n" << entityfy::migrate::schema_to_json(composite) << "\nactual:\n"
                      << entityfy::migrate::schema_to_json(actual) << "\n";
        }
        require(entityfy::migrate::same_schema(composite, actual), "composite create table roundtrip");

        const auto* actual_lines = entityfy::migrate::find_table(actual, "key_lines");
        require(actual_lines != nullptr, "composite table introspected");
        require(actual_lines->primary_key == std::vector<std::string>{"id", "region"},
                "composite key order introspected");

        const auto* actual_notes = entityfy::migrate::find_table(actual, "key_notes");
        require(actual_notes != nullptr, "single key table introspected");
        require(entityfy::migrate::primary_key_columns(*actual_notes) == std::vector<std::string>{"id"},
                "single key column introspected");

        const auto idempotent = composite_backend->make_plan(composite, actual, {});
        require(idempotent.steps.empty(), "second composite apply has no steps");

        composite_backend->execute_sql("INSERT INTO \"key_lines\" (\"id\", \"region\", \"quantity\") VALUES (1, 'eu', 3)");

        bool duplicate_rejected = false;
        try
        {
            composite_backend->execute_sql(
                "INSERT INTO \"key_lines\" (\"id\", \"region\", \"quantity\") VALUES (1, 'eu', 9)");
        }
        catch(const std::exception&)
        {
            duplicate_rejected = true;
        }

        require(duplicate_rejected, "composite key rejects duplicate rows");

        composite_backend->execute_sql("INSERT INTO \"key_lines\" (\"id\", \"region\", \"quantity\") VALUES (1, 'us', 4)");
        rows = composite_backend->query("SELECT COUNT(*) FROM \"key_lines\"");
        require(rows.size() == 1 && rows.front()[0] == "2", "different regions are distinct keys");
    }

    return entityfy_test::status::passed;
}
