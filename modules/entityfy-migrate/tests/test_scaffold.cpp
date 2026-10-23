#include "harness.hpp"

#include <entityfy/migrate/json.hpp>
#include <entityfy/migrate/scaffold.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

entityfy_test::status test_scaffold()
{
    entityfy::migrate::schema value;

    entityfy::migrate::table users;
    users.name = "users";
    users.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true,
                                        .primary_key = true, .auto_increment = true});
    users.columns.push_back(entityfy::migrate::column{.name = "email", .type = "VARCHAR(320)", .not_null = true,
                                        .unique = true, .indexed = true});
    users.columns.push_back(entityfy::migrate::column{.name = "created_at", .type = "TIMESTAMP", .not_null = true,
                                        .default_sql = "CURRENT_TIMESTAMP"});
    users.columns.push_back(entityfy::migrate::column{.name = "class", .type = "TEXT"});
    users.columns.push_back(entityfy::migrate::column{.name = "user-id", .type = "TEXT"});
    users.columns.push_back(entityfy::migrate::column{.name = "user_id", .type = "TEXT"});
    users.indexes.push_back(entityfy::migrate::index{.name = "uidx_users_email", .columns = {"email"}, .unique = true});
    value.tables.push_back(users);

    entityfy::migrate::table posts;
    posts.name = "posts";
    posts.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .primary_key = true,
                                        .auto_increment = true});
    posts.columns.push_back(entityfy::migrate::column{.name = "user_id", .type = "INTEGER", .not_null = true});
    posts.columns.back().references = entityfy::migrate::reference{.table = "users", .column = "id", .on_delete = "CASCADE"};
    posts.foreign_keys.push_back(
        entityfy::migrate::foreign_key{.column = "user_id", .ref_table = "users", .ref_column = "id", .on_delete = "CASCADE"});
    value.tables.push_back(posts);

    entityfy::migrate::table numeric;
    numeric.name = "123_users";
    numeric.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .primary_key = true,
                                          .auto_increment = true});
    value.tables.push_back(numeric);

    entityfy::migrate::table regions;
    regions.name = "key_regions";
    regions.primary_key = {"id", "region"};
    regions.columns.push_back(
        entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true, .primary_key = true});
    regions.columns.push_back(
        entityfy::migrate::column{.name = "region", .type = "TEXT", .not_null = true, .primary_key = true});
    regions.columns.push_back(entityfy::migrate::column{.name = "title", .type = "TEXT"});
    value.tables.push_back(regions);

    const auto text = entityfy::migrate::generate_models(value, "shop");

    require(text.find("namespace shop") != std::string::npos, "scaffold namespace");
    require(text.find("struct [[= entityfy::table{\"users\"}]] User") != std::string::npos,
            "scaffold table struct");
    require(text.find("entityfy::primary_key") != std::string::npos, "scaffold primary key");
    require(text.find("entityfy::auto_increment") != std::string::npos, "scaffold auto increment");
    require(text.find("entityfy::unique") != std::string::npos, "scaffold unique index");
    require(text.find("entityfy::varchar{320}") != std::string::npos, "scaffold varchar length");
    require(text.find("entityfy::references{\"users\", \"id\", \"CASCADE\"}") != std::string::npos,
            "scaffold foreign key");
    require(text.find("std::chrono::system_clock::time_point") != std::string::npos,
            "scaffold timestamp type");
    require(text.find("entityfy::default_value{\"CURRENT_TIMESTAMP\"}") != std::string::npos,
            "scaffold default value");
    require(text.find("struct [[= entityfy::table{\"posts\"}]] Post") != std::string::npos,
            "scaffold second table");
    require(text.find("struct [[= entityfy::table{\"123_users\"}]] _123User") != std::string::npos,
            "scaffold sanitizes digit-leading table name");

    require(text.find("std::string class_") != std::string::npos, "scaffold escapes C++ keyword");
    require(text.find("std::string user_id{}") != std::string::npos, "scaffold sanitizes punctuation");
    require(text.find("std::string user_id_2{}") != std::string::npos, "scaffold deduplicates members");

    const auto keyword_namespace = entityfy::migrate::generate_models(value, "class");
    require(keyword_namespace.find("namespace class_") != std::string::npos,
            "scaffold escapes keyword namespace");

    entityfy::migrate::schema postgres;
    entityfy::migrate::table accounts;
    accounts.name = "accounts";
    accounts.columns.push_back(entityfy::migrate::column{.name = "id", .type = "BIGINT", .primary_key = true,
                                           .auto_increment = true});
    accounts.columns.push_back(entityfy::migrate::column{.name = "email", .type = "CHARACTER VARYING(320)", .not_null = true});
    accounts.columns.push_back(entityfy::migrate::column{.name = "created_at", .type = "TIMESTAMP WITHOUT TIME ZONE"});
    accounts.columns.push_back(entityfy::migrate::column{.name = "token", .type = "UUID"});
    accounts.columns.push_back(entityfy::migrate::column{.name = "payload", .type = "JSONB"});
    accounts.columns.push_back(entityfy::migrate::column{.name = "raw", .type = "BYTEA"});
    postgres.tables.push_back(accounts);

    const auto pg_text = entityfy::migrate::generate_models(postgres, "pg_models");
    require(pg_text.find("entityfy::varchar{320}") != std::string::npos, "pg scaffold varchar");
    require(pg_text.find("std::chrono::system_clock::time_point") != std::string::npos,
            "pg scaffold timestamp");
    require(pg_text.find("entityfy::uuid") != std::string::npos, "pg scaffold uuid");
    require(pg_text.find("entityfy::json") != std::string::npos, "pg scaffold json");
    require(pg_text.find("std::vector<std::byte>") != std::string::npos, "pg scaffold bytea");
    require(pg_text.find("entityfy::blob") != std::string::npos, "pg scaffold blob annotation");

    // A composite key must annotate every key column, in the same style as a
    // single-column key, and the generated text must be deterministic.
    require(text.find("struct [[= entityfy::table{\"key_regions\"}]] KeyRegion") != std::string::npos,
            "scaffold composite struct");

    entityfy::migrate::schema composite;
    composite.tables.push_back(regions);
    const auto composite_text = entityfy::migrate::generate_models(composite, "composite_models");

    const auto count_key_annotations = [](const std::string& generated) {
        std::size_t count = 0;
        const std::string needle = "= entityfy::primary_key";
        for(auto position = generated.find(needle); position != std::string::npos;
            position = generated.find(needle, position + needle.size()))
        {
            ++count;
        }
        return count;
    };

    require(composite_text.find("struct [[= entityfy::table{\"key_regions\"}]] KeyRegion") != std::string::npos,
            "scaffold composite struct isolated");
    require(count_key_annotations(composite_text) == 2, "scaffold emits one key annotation per composite column");
    require(composite_text.find("[[= entityfy::primary_key]]\n    std::int64_t id = 0;") != std::string::npos,
            "scaffold annotates the first key column");
    require(composite_text.find("[[= entityfy::primary_key, = entityfy::text]]\n    std::string region{};") !=
                std::string::npos,
            "scaffold annotates the second key column");
    require(composite_text.find("entityfy::not_null") == std::string::npos,
            "scaffold does not duplicate not_null on key columns");
    require(composite_text == entityfy::migrate::generate_models(composite, "composite_models"),
            "scaffold composite output is stable");
    require(composite_text ==
                entityfy::migrate::generate_models(
                    entityfy::migrate::schema_from_json(entityfy::migrate::schema_to_json(composite)),
                    "composite_models"),
            "scaffold composite output survives a json roundtrip");

#ifdef ENTITYFY_CORE_INCLUDE
    {
        const char* path = "scaffold_generated.hpp";
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream << text;
        stream.close();

        const std::string command = "g++ -std=c++26 -freflection -fsyntax-only -I \"" +
                                    std::string{ENTITYFY_CORE_INCLUDE} + "\" " + path;
        const int result = std::system(command.c_str());
        require(result == 0, "scaffold generated header compiles");

        std::filesystem::remove(path);
    }
#endif

    return entityfy_test::status::passed;
}
