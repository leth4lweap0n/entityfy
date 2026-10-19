// Demonstrates entityfy ORM migration behavior concretely:
//  1) the v1 schema is created and one row is inserted
//  2) a new column+index is added to the v2 model -> plan_migration produces ADD COLUMN + CREATE INDEX
//  3) migrate_versioned writes the id into entityfy_schema_migrations
//  4) v3 changes a type -> no additive plan, validation reports drift
//  5) v4 renames a column -> no rename; a new column is added, the old one remains as extra_column

#include <entityfy/entityfy.hpp>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

struct [[= entityfy::table{"users"}]] UserV1
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::unique, = entityfy::indexed, = entityfy::not_null, = entityfy::varchar{320}]]
    std::string email;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;

    [[= entityfy::created_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point created_at{};

    [[= entityfy::updated_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point updated_at{};
};

struct [[= entityfy::table{"users"}]] UserV2
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::unique, = entityfy::indexed, = entityfy::not_null, = entityfy::varchar{320}]]
    std::string email;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::varchar{40}, = entityfy::default_value{"'none'"}]]
    std::string nickname;

    [[= entityfy::created_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point created_at{};

    [[= entityfy::updated_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point updated_at{};
};

struct [[= entityfy::table{"users"}]] UserV3
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::unique, = entityfy::indexed, = entityfy::not_null, = entityfy::varchar{100}]]
    std::string email;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::varchar{40}, = entityfy::default_value{"'none'"}]]
    std::string nickname;

    [[= entityfy::created_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point created_at{};

    [[= entityfy::updated_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point updated_at{};
};

struct [[= entityfy::table{"users"}]] UserV4
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::unique, = entityfy::indexed, = entityfy::not_null, = entityfy::varchar{320}]]
    std::string email;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string full_name;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::varchar{40}, = entityfy::default_value{"'none'"}]]
    std::string nickname;

    [[= entityfy::created_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point created_at{};

    [[= entityfy::updated_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point updated_at{};
};

struct [[= entityfy::table{"entityfy_schema_migrations"}]] AppliedMigration
{
    [[= entityfy::primary_key]]
    std::string id;

    std::string applied_at;
};

namespace
{

std::vector<std::string> columns_of(entityfy::client& db, std::string_view table)
{
    std::vector<std::string> names;
    for(const auto& column: db.inspect_table(table).columns)
    {
        names.push_back(column.name);
    }
    return names;
}

void print_columns(entityfy::client& db, std::string_view table)
{
    const auto info = db.inspect_table(table);
    std::cout << "  columns:";
    for(const auto& column: info.columns)
    {
        std::cout << " " << column.name << "(" << column.sql_type << ")";
    }
    std::cout << "\n";
}

void print_plan(const entityfy::migration_plan& plan)
{
    std::cout << "  plan: " << plan.statements.size() << " statements\n";
    for(const auto& statement: plan.statements)
    {
        std::cout << "    " << statement.sql << "\n";
    }
}

void print_issues(const entityfy::schema_validation_result& validation)
{
    std::cout << "  validation: " << (validation.valid() ? "PASSED" : "DRIFT") << "\n";
    for(const auto& issue: validation.issues)
    {
        std::cout << "    - " << issue.message << " (" << issue.object_name << ")\n";
    }
}

} // namespace

int main()
{
    std::remove("migrate_probe.db");

    entityfy::client db{"sqlite://migrate_probe.db"};
    const entityfy::dialect target = db.target_dialect();

    auto v1 = db.table<UserV1>();
    v1.migrate();
    (void)v1.insert(UserV1{.email = "ada@test.com", .name = "Ada"});

    std::cout << "[v1] initial schema created (1 row inserted)\n";
    print_columns(db, "users");

    std::cout << "\n[v1 -> v2] plan_migration output (nickname + index):\n";
    print_plan(entityfy::plan_migration(entityfy::describe_model<UserV2>(target), columns_of(db, "users")));

    auto v2 = db.table<UserV2>();
    v2.migrate();
    std::cout << "[v2] migrate() applied\n";
    print_columns(db, "users");

    v2.migrate_versioned("v2_add_nickname");
    auto migrations = db.table<AppliedMigration>().find_many();
    std::cout << "[versioned] entityfy_schema_migrations records:";
    for(const auto& entry: migrations)
    {
        std::cout << " " << entry.id;
    }
    std::cout << "\n";

    std::cout << "\n[v3] email VARCHAR(320) -> VARCHAR(100), additive plan:\n";
    print_plan(entityfy::plan_migration(entityfy::describe_model<UserV3>(target), columns_of(db, "users")));
    print_issues(db.table<UserV3>().validate_schema(entityfy::schema_validation_options{.strict_sql_types = true}));

    std::cout << "\n[v4] name -> full_name rename, additive plan:\n";
    print_plan(entityfy::plan_migration(entityfy::describe_model<UserV4>(target), columns_of(db, "users")));
    print_issues(db.table<UserV4>().validate_schema());

    return 0;
}
