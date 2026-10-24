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

entityfy::migrate::schema composite_schema()
{
    entityfy::migrate::schema output;

    entityfy::migrate::table parents;
    parents.name = "cr_parents";
    parents.primary_key = {"tenant_id", "code"};
    parents.columns.push_back(
        entityfy::migrate::column{.name = "tenant_id", .type = "INTEGER", .not_null = true, .primary_key = true});
    parents.columns.push_back(
        entityfy::migrate::column{.name = "code", .type = "TEXT", .not_null = true, .primary_key = true});
    parents.columns.push_back(entityfy::migrate::column{.name = "name", .type = "TEXT", .not_null = true});
    output.tables.push_back(parents);

    entityfy::migrate::table children;
    children.name = "cr_children";
    children.primary_key = {"id"};
    children.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true,
                                                         .primary_key = true, .auto_increment = true});
    children.columns.push_back(entityfy::migrate::column{.name = "tenant_id", .type = "INTEGER", .not_null = true});
    children.columns.push_back(entityfy::migrate::column{.name = "parent_code", .type = "TEXT", .not_null = true});
    children.columns.push_back(entityfy::migrate::column{.name = "body", .type = "TEXT", .not_null = true});
    children.foreign_keys.push_back(entityfy::migrate::foreign_key{
        .column = "tenant_id",
        .ref_table = "cr_parents",
        .ref_column = "tenant_id",
        .on_delete = "CASCADE",
        .on_update = "CASCADE",
        .columns = {"tenant_id", "parent_code"},
        .ref_columns = {"tenant_id", "code"},
    });
    output.tables.push_back(children);

    return output;
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

    {
        const auto desired = composite_schema();
        const auto actual = desired;

        const auto operations = entityfy::migrate::diff_schemas(desired, actual);
        require(operations.empty(), "identical composite foreign keys produce no operations");
    }

    {
        auto desired = composite_schema();
        auto actual = desired;

        auto* actual_child = entityfy::migrate::find_table(actual, "cr_children");
        actual_child->foreign_keys.front().ref_columns = {"tenant_id", "tenant_id"};

        const auto operations = entityfy::migrate::diff_schemas(desired, actual);
        require(has_kind(operations, entityfy::migrate::op_kind::add_foreign_key),
                "composite foreign key add detected");
        require(has_kind(operations, entityfy::migrate::op_kind::drop_foreign_key),
                "composite foreign key drop detected");
        require(operations.size() == 2, "composite foreign key change is one drop and one add");

        const auto detail = std::ranges::find_if(operations, [](const entityfy::migrate::operation& value) {
            return value.kind == entityfy::migrate::op_kind::add_foreign_key;
        });
        require(detail != operations.end() &&
                    detail->detail.find("tenant_id, parent_code") != std::string::npos &&
                    detail->detail.find("tenant_id, code") != std::string::npos,
                "composite foreign key detail names both column lists");
    }

    {
        auto desired = composite_schema();
        auto actual = desired;

        auto* actual_child = entityfy::migrate::find_table(actual, "cr_children");
        actual_child->foreign_keys.front().columns = {"parent_code", "tenant_id"};

        const auto operations = entityfy::migrate::diff_schemas(desired, actual);
        require(has_kind(operations, entityfy::migrate::op_kind::add_foreign_key) &&
                    has_kind(operations, entityfy::migrate::op_kind::drop_foreign_key),
                "composite foreign key column order is significant");
    }

    {
        const auto composite = composite_schema();
        bool rejected = false;

        try
        {
            entityfy::migrate::require_single_column_foreign_keys(composite, "mysql");
        }
        catch(const std::logic_error& error)
        {
            rejected = std::string{error.what()}.find("composite foreign keys are not supported") != std::string::npos &&
                       std::string{error.what()}.find("cr_children") != std::string::npos;
        }

        require(rejected, "unsupported provider rejects a composite foreign key by name");

        auto single = composite_schema();
        auto* child = entityfy::migrate::find_table(single, "cr_children");
        child->foreign_keys.front().columns = {"tenant_id"};
        child->foreign_keys.front().ref_columns = {"tenant_id"};

        entityfy::migrate::require_single_column_foreign_keys(single, "mysql");
    }

    return entityfy_test::status::passed;
}
