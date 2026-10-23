#include "harness.hpp"

#include <entityfy/migrate/schema.hpp>
#include <entityfy/migrate/sha256.hpp>

#include <vector>

entityfy_test::status test_schema()
{
    require(entityfy::migrate::normalize_sql(" varchar(320) ") == "VARCHAR(320)", "normalize type");
    require(entityfy::migrate::normalize_sql("VARCHAR(320)") == entityfy::migrate::normalize_sql("varchar ( 320 )"), "normalize spacing");
    require(entityfy::migrate::same_type("INTEGER", "integer"), "same type case");
    require(!entityfy::migrate::same_type("INTEGER", "TEXT"), "different types");

    require(entityfy::migrate::quote_ident("users") == "\"users\"", "quote identifier");
    require(entityfy::migrate::quote_ident("we\"ird") == "\"we\"\"ird\"", "escape identifier");
    require(entityfy::migrate::quote_literal("it's") == "'it''s'", "escape literal");

    require(entityfy::migrate::sha256_hex("") ==
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "sha256 empty");
    require(entityfy::migrate::sha256_hex("abc") ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "sha256 abc");

    entityfy::migrate::schema left;
    entityfy::migrate::table users;
    users.name = "users";
    users.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true, .primary_key = true});
    users.columns.push_back(entityfy::migrate::column{.name = "email", .type = "VARCHAR(320)", .not_null = true});
    left.tables.push_back(users);

    auto right = left;
    require(entityfy::migrate::same_schema(left, right), "same schema");

    right.tables.front().columns.front().type = "BIGINT";
    require(!entityfy::migrate::same_schema(left, right), "type drift detected");

    right = left;
    right.tables.front().columns.push_back(entityfy::migrate::column{.name = "extra", .type = "TEXT"});
    require(!entityfy::migrate::same_schema(left, right), "column drift detected");

    entityfy::migrate::schema composite;
    entityfy::migrate::table regions;
    regions.name = "key_regions";
    regions.primary_key = {"id", "region"};
    regions.columns.push_back(
        entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true, .primary_key = true});
    regions.columns.push_back(
        entityfy::migrate::column{.name = "region", .type = "TEXT", .not_null = true, .primary_key = true});
    composite.tables.push_back(regions);

    auto composite_copy = composite;
    require(entityfy::migrate::same_schema(composite, composite_copy), "same composite schema");

    composite_copy.tables.front().primary_key = {"region", "id"};
    require(!entityfy::migrate::same_schema(composite, composite_copy), "reversed key order detected");

    composite_copy = composite;
    composite_copy.tables.front().columns.push_back(
        entityfy::migrate::column{.name = "tenant", .type = "TEXT", .not_null = true, .primary_key = true});
    composite_copy.tables.front().primary_key = {"id", "region", "tenant"};
    require(!entityfy::migrate::same_schema(composite, composite_copy), "extra key column detected");

    // Old snapshots carry only the per-column flags; the derived list must
    // compare equal to the explicit one.
    auto flagged_only = composite;
    flagged_only.tables.front().primary_key.clear();
    require(entityfy::migrate::primary_key_columns(flagged_only.tables.front()) ==
                entityfy::migrate::primary_key_columns(composite.tables.front()),
            "flag fallback matches the explicit key list");
    require(entityfy::migrate::same_schema(composite, flagged_only), "explicit and derived key lists compare equal");

    return entityfy_test::status::passed;
}
