#include "harness.hpp"

#include <entityfy/migrate/json.hpp>
#include <entityfy/migrate/sha256.hpp>

#include <string>
#include <vector>

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

    // A composite key keeps its declared order through JSON, even when that
    // order differs from the column declaration order.
    entityfy::migrate::table regions;
    regions.name = "key_regions";
    regions.primary_key = {"region", "id"};
    regions.columns.push_back(
        entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true, .primary_key = true});
    regions.columns.push_back(
        entityfy::migrate::column{.name = "region", .type = "TEXT", .not_null = true, .primary_key = true});

    entityfy::migrate::schema composite;
    composite.tables.push_back(regions);

    const std::string composite_text = entityfy::migrate::schema_to_json(composite);
    const auto composite_back = entityfy::migrate::schema_from_json(composite_text);
    const auto* restored_regions = entityfy::migrate::find_table(composite_back, "key_regions");
    require(restored_regions != nullptr, "composite table restored");
    require(restored_regions->primary_key == std::vector<std::string>{"region", "id"},
            "composite key order roundtrip");
    require(entityfy::migrate::primary_key_columns(*restored_regions) == std::vector<std::string>{"region", "id"},
            "composite key helper roundtrip");
    require(entityfy::migrate::same_schema(composite, composite_back), "composite schema roundtrip");
    require(composite_text == entityfy::migrate::schema_to_json(composite_back), "composite json is stable");

    // A snapshot written before `table::primary_key` existed only carries the
    // per-column flags; the ordered list is derived from the column order.
    const std::string legacy_text = R"({
  "version": 1,
  "tables": [
    {
      "name": "key_regions",
      "columns": [
        {"name": "id", "type": "INTEGER", "not_null": true, "primary_key": true},
        {"name": "region", "type": "TEXT", "not_null": true, "primary_key": true},
        {"name": "title", "type": "TEXT", "not_null": false, "primary_key": false}
      ],
      "indexes": [],
      "foreign_keys": []
    }
  ]
})";

    const auto legacy = entityfy::migrate::schema_from_json(legacy_text);
    const auto* legacy_regions = entityfy::migrate::find_table(legacy, "key_regions");
    require(legacy_regions != nullptr, "legacy snapshot table restored");
    require(legacy_regions->primary_key.empty(), "legacy snapshot has no explicit key list");
    require(entityfy::migrate::primary_key_columns(*legacy_regions) == std::vector<std::string>{"id", "region"},
            "legacy snapshot derives the ordered key");
    require(entityfy::migrate::schema_to_json(entityfy::migrate::schema_from_json(entityfy::migrate::schema_to_json(legacy))) ==
                entityfy::migrate::schema_to_json(legacy),
            "legacy snapshot json is stable");

    return entityfy_test::status::passed;
}
