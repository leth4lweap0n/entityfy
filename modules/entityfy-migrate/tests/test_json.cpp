#include "harness.hpp"

#include <entityfy/migrate/json.hpp>
#include <entityfy/migrate/sha256.hpp>

entityfy_test::status test_json()
{
    entityfy::migrate::schema value;

    entityfy::migrate::table users;
    users.name = "users";
    users.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true, .primary_key = true,
                                        .auto_increment = true});
    users.columns.push_back(entityfy::migrate::column{.name = "email", .type = "VARCHAR(320)", .not_null = true, .unique = true});
    users.columns.back().references = entityfy::migrate::reference{.table = "accounts", .column = "id", .on_delete = "CASCADE"};
    users.indexes.push_back(entityfy::migrate::index{.name = "uidx_users_email", .columns = {"email"}, .unique = true});
    users.foreign_keys.push_back(entityfy::migrate::foreign_key{.column = "email", .ref_table = "accounts", .ref_column = "id"});
    value.tables.push_back(users);

    const std::string text = entityfy::migrate::schema_to_json(value);
    const auto parsed = entityfy::migrate::schema_from_json(text);

    require(entityfy::migrate::same_schema(value, parsed), "schema json roundtrip");
    require(text == entityfy::migrate::schema_to_json(parsed), "schema json is stable");

    entityfy::migrate::migration_file file;
    file.id = "20260101000000_init";
    file.created_at = "20260101000000";
    file.provider = "sqlite";
    file.before = entityfy::migrate::schema{};
    file.after = value;
    file.checksum = entityfy::migrate::sha256_hex(entityfy::migrate::schema_to_json(file.before) + entityfy::migrate::schema_to_json(file.after));

    const auto restored = entityfy::migrate::migration_from_json(entityfy::migrate::migration_to_json(file));
    require(restored.id == file.id, "migration id roundtrip");
    require(restored.provider == file.provider, "migration provider roundtrip");
    require(restored.checksum == file.checksum, "migration checksum roundtrip");
    require(entityfy::migrate::same_schema(restored.after, file.after), "migration schema roundtrip");

    return entityfy_test::status::passed;
}
