#include "harness.hpp"

#include <entityfy/migrate/diff.hpp>

#include <algorithm>

namespace
{

entityfy::migrate::schema base_schema()
{
    entityfy::migrate::schema output;

    entityfy::migrate::table users;
    users.name = "users";
    users.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true, .primary_key = true,
                                        .auto_increment = true});
    users.columns.push_back(entityfy::migrate::column{.name = "email", .type = "VARCHAR(320)", .not_null = true});
    users.columns.push_back(entityfy::migrate::column{.name = "name", .type = "VARCHAR(120)", .not_null = true});
    users.indexes.push_back(entityfy::migrate::index{.name = "uidx_users_email", .columns = {"email"}, .unique = true});
    output.tables.push_back(users);

    return output;
}

bool has_kind(const std::vector<entityfy::migrate::operation>& operations, entityfy::migrate::op_kind kind)
{
    return std::ranges::any_of(operations, [&](const entityfy::migrate::operation& value) {
        return value.kind == kind;
    });
}

} // namespace

entityfy_test::status test_diff()
{
    const auto actual = base_schema();

    {
        auto desired = actual;
        entityfy::migrate::find_table(desired, "users")->columns.push_back(
            entityfy::migrate::column{.name = "nickname", .type = "VARCHAR(40)"});

        const auto operations = entityfy::migrate::diff_schemas(desired, actual);
        require(operations.size() == 1, "add column operation count");
        require(operations.front().kind == entityfy::migrate::op_kind::add_column, "add column kind");
        require(!operations.front().destructive, "add column not destructive");
    }

    {
        auto desired = actual;
        auto* users = entityfy::migrate::find_table(desired, "users");
        users->columns.erase(std::ranges::find_if(users->columns, [](const entityfy::migrate::column& value) {
            return value.name == "name";
        }));

        entityfy::migrate::diff_options options;
        options.renames["users.name"] = "full_name";
        users->columns.push_back(entityfy::migrate::column{.name = "full_name", .type = "VARCHAR(120)", .not_null = true});

        const auto operations = entityfy::migrate::diff_schemas(desired, actual, options);
        require(has_kind(operations, entityfy::migrate::op_kind::rename_column), "rename detected");
        require(!has_kind(operations, entityfy::migrate::op_kind::drop_column), "rename did not drop");
        require(!has_kind(operations, entityfy::migrate::op_kind::add_column), "rename did not add");
    }

    {
        auto desired = actual;
        auto* users = entityfy::migrate::find_table(desired, "users");
        users->columns.erase(std::ranges::find_if(users->columns, [](const entityfy::migrate::column& value) {
            return value.name == "name";
        }));

        const auto operations = entityfy::migrate::diff_schemas(desired, actual);
        require(has_kind(operations, entityfy::migrate::op_kind::drop_column), "drop detected");
        require(entityfy::migrate::has_destructive(operations), "drop destructive");
    }

    {
        auto desired = actual;
        auto* users = entityfy::migrate::find_table(desired, "users");
        entityfy::migrate::find_column(*users, "email")->type = "VARCHAR(100)";

        const auto operations = entityfy::migrate::diff_schemas(desired, actual);
        require(has_kind(operations, entityfy::migrate::op_kind::alter_column), "alter detected");
        require(entityfy::migrate::has_destructive(operations), "type change destructive");
    }

    {
        auto desired = actual;
        entityfy::migrate::table posts;
        posts.name = "posts";
        posts.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true,
                                            .primary_key = true, .auto_increment = true});
        desired.tables.push_back(posts);

        const auto operations = entityfy::migrate::diff_schemas(desired, actual);
        require(operations.size() == 1 && operations.front().kind == entityfy::migrate::op_kind::create_table,
                "create table detected");
        require(!entityfy::migrate::has_destructive(operations), "create table not destructive");

        const auto reverse = entityfy::migrate::diff_schemas(actual, desired);
        require(has_kind(reverse, entityfy::migrate::op_kind::drop_table), "drop table detected");
        require(entityfy::migrate::has_destructive(reverse), "drop table destructive");
    }

    return entityfy_test::status::passed;
}
