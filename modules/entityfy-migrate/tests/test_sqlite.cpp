#include "harness.hpp"

#include <entityfy/migrate/provider.hpp>
#include <entityfy/migrate/json.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

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

    return entityfy_test::status::passed;
}
