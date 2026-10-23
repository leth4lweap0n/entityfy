#include "support/Fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

TEST_CASE("insert skips generated columns and update touches updated_at", "[statement]")
{
    const auto model = entityfy::describe_model<entityfy::test::User>(entityfy::dialect::sqlite);
    const entityfy::test::User user{
        .email = "ada@example.test",
        .name = "Ada",
    };

    const auto insert = entityfy::insert_statement(user, model);
    REQUIRE(insert.sql.find("\"created_at\"") == std::string::npos);
    REQUIRE(insert.sql.find("\"updated_at\"") == std::string::npos);
    REQUIRE(insert.binds.size() == 2);

    const entityfy::test::User existing{
        .id = 42,
        .email = "ada@example.test",
        .name = "Ada",
    };

    const auto update = entityfy::update_statement(existing, model);
    REQUIRE(update.sql.find("\"created_at\"") == std::string::npos);
    REQUIRE(update.sql.find("\"updated_at\" = CURRENT_TIMESTAMP") != std::string::npos);
    REQUIRE(update.sql.find("WHERE \"id\" = ?") != std::string::npos);
    REQUIRE(update.binds.size() == 3);
}

TEST_CASE("update_many emits guarded set clauses", "[statement]")
{
    const auto model = entityfy::describe_model<entityfy::test::User>(entityfy::dialect::sqlite);
    const entityfy::test::User patch{
        .email = "new@example.test",
        .name = "New",
    };

    const auto update = entityfy::update_many_statement(
        patch,
        model,
        entityfy::where(&entityfy::test::User::email).contains("example")
    );

    REQUIRE(update.sql.find("UPDATE \"users\" SET") != std::string::npos);
    REQUIRE(update.sql.find("\"created_at\"") == std::string::npos);
    REQUIRE(update.sql.find("WHERE \"email\" LIKE ?") != std::string::npos);
    REQUIRE(update.sql.find("\"updated_at\" = CURRENT_TIMESTAMP") != std::string::npos);
    REQUIRE(update.binds.size() == 3);
}

TEST_CASE("upsert emits primary-key conflict update", "[statement]")
{
    const entityfy::test::User user{
        .id = 42,
        .email = "ada@example.test",
        .name = "Ada",
    };

    const auto sqlite_model = entityfy::describe_model<entityfy::test::User>(entityfy::dialect::sqlite);
    const auto sqlite = entityfy::upsert_statement(user, sqlite_model);

    REQUIRE(sqlite.sql.find("INSERT INTO \"users\"") != std::string::npos);
    REQUIRE(sqlite.sql.find("\"id\", \"email\", \"name\"") != std::string::npos);
    REQUIRE(sqlite.sql.find("ON CONFLICT (\"id\") DO UPDATE SET") != std::string::npos);
    REQUIRE(sqlite.sql.find("\"email\" = excluded.\"email\"") != std::string::npos);
    REQUIRE(sqlite.sql.find("\"name\" = excluded.\"name\"") != std::string::npos);
    REQUIRE(sqlite.sql.find("\"updated_at\" = CURRENT_TIMESTAMP") != std::string::npos);
    REQUIRE(sqlite.sql.find("\"created_at\"") == std::string::npos);
    REQUIRE(sqlite.binds.size() == 3);

    const auto postgres_model = entityfy::describe_model<entityfy::test::User>(entityfy::dialect::postgresql);
    const auto postgres = entityfy::upsert_statement(user, postgres_model);

    REQUIRE(postgres.sql.find("ON CONFLICT (\"id\") DO UPDATE SET") != std::string::npos);
    REQUIRE(postgres.sql.find("\"updated_at\" = CURRENT_TIMESTAMP") != std::string::npos);
    REQUIRE(postgres.binds.size() == 3);
}

namespace
{

struct [[= entityfy::table{"key_regions"}]] composite_region
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::primary_key, = entityfy::varchar{32}]]
    std::string region;

    [[= entityfy::not_null, = entityfy::varchar{200}]]
    std::string title;

    [[= entityfy::not_null]]
    std::int64_t score = 0;
};

// The key column is renamed by an annotation, so the key predicate has to use
// the descriptor name instead of the member name.
struct [[= entityfy::table{"key_aliased_states"}]] aliased_state
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::primary_key, = entityfy::column{"region_code"}, = entityfy::varchar{16}]]
    std::string region;

    [[= entityfy::not_null]]
    std::string note;
};

} // namespace

TEST_CASE("select by composite key binds every key part in descriptor order", "[statement][primary_key]")
{
    const auto model = entityfy::describe_model<composite_region>(entityfy::dialect::sqlite);
    const entityfy::row_key key{{entityfy::sql_value{std::int64_t{7}}, entityfy::sql_value{std::string{"eu"}}}};

    const auto select = entityfy::select_by_key_statement(model, key);
    REQUIRE(select.sql ==
            "SELECT \"id\", \"region\", \"title\", \"score\" FROM \"key_regions\" "
            "WHERE \"id\" = ? AND \"region\" = ? LIMIT 1");
    REQUIRE(select.binds.size() == 2);
    REQUIRE(std::get<std::int64_t>(select.binds[0]) == 7);
    REQUIRE(std::get<std::string>(select.binds[1]) == "eu");
}

TEST_CASE("a row_key that does not match the model key is rejected", "[statement][primary_key]")
{
    const auto model = entityfy::describe_model<composite_region>(entityfy::dialect::sqlite);

    // An empty key has no binds to spread over the two key columns.
    REQUIRE_THROWS_AS(entityfy::select_by_key_statement(model, entityfy::row_key{}), std::logic_error);

    // Too few parts.
    REQUIRE_THROWS_AS(
        entityfy::select_by_key_statement(model, entityfy::row_key{{entityfy::sql_value{std::int64_t{7}}}}),
        std::logic_error);

    // Too many parts.
    REQUIRE_THROWS_AS(
        entityfy::select_by_key_statement(
            model,
            entityfy::row_key{{entityfy::sql_value{std::int64_t{7}},
                               entityfy::sql_value{std::string{"eu"}},
                               entityfy::sql_value{std::int64_t{3}}}}),
        std::logic_error);

    // A NULL part is a caller error, not a SQL `IS NULL` predicate.
    REQUIRE_THROWS_AS(
        entityfy::select_by_key_statement(
            model,
            entityfy::row_key{{entityfy::sql_value{}, entityfy::sql_value{std::string{"eu"}}}}),
        std::logic_error);

    REQUIRE_THROWS_AS(entityfy::delete_by_key_statement(model, entityfy::row_key{}), std::logic_error);
}

TEST_CASE("update by composite key writes the SET columns then the key parts", "[statement][primary_key]")
{
    const auto model = entityfy::describe_model<composite_region>(entityfy::dialect::sqlite);
    const composite_region row{.id = 7, .region = "eu", .title = "Renamed", .score = 3};

    const auto update = entityfy::update_statement(row, model);
    REQUIRE(update.sql ==
            "UPDATE \"key_regions\" SET \"title\" = ?, \"score\" = ? "
            "WHERE \"id\" = ? AND \"region\" = ?");
    REQUIRE(update.binds.size() == 4);
    REQUIRE(std::get<std::string>(update.binds[0]) == "Renamed");
    REQUIRE(std::get<std::int64_t>(update.binds[1]) == 3);
    REQUIRE(std::get<std::int64_t>(update.binds[2]) == 7);
    REQUIRE(std::get<std::string>(update.binds[3]) == "eu");
}

TEST_CASE("single-key update SQL keeps its exact previous text", "[statement][primary_key]")
{
    const auto model = entityfy::describe_model<entityfy::test::User>(entityfy::dialect::sqlite);
    const entityfy::test::User existing{.id = 42, .email = "ada@example.test", .name = "Ada"};

    const auto update = entityfy::update_statement(existing, model);
    REQUIRE(update.sql ==
            "UPDATE \"users\" SET \"email\" = ?, \"name\" = ?, \"updated_at\" = CURRENT_TIMESTAMP WHERE \"id\" = ?");
    REQUIRE(update.binds.size() == 3);
    REQUIRE(std::get<std::string>(update.binds[0]) == "ada@example.test");
    REQUIRE(std::get<std::string>(update.binds[1]) == "Ada");
    REQUIRE(std::get<std::int64_t>(update.binds[2]) == 42);
}

TEST_CASE("delete by composite key binds the key parts and then the token", "[statement][primary_key]")
{
    const auto model = entityfy::describe_model<composite_region>(entityfy::dialect::sqlite);
    const entityfy::row_key key{{entityfy::sql_value{std::int64_t{7}}, entityfy::sql_value{std::string{"eu"}}}};

    const auto plain = entityfy::delete_by_key_statement(model, key);
    REQUIRE(plain.sql == "DELETE FROM \"key_regions\" WHERE \"id\" = ? AND \"region\" = ?");
    REQUIRE(plain.binds.size() == 2);
    REQUIRE(std::get<std::int64_t>(plain.binds[0]) == 7);
    REQUIRE(std::get<std::string>(plain.binds[1]) == "eu");

    const auto guarded =
        entityfy::delete_by_key_statement(model, key, "score", entityfy::sql_value{std::int64_t{4}});
    REQUIRE(guarded.sql == "DELETE FROM \"key_regions\" WHERE \"id\" = ? AND \"region\" = ? AND \"score\" = ?");
    REQUIRE(guarded.binds.size() == 3);
    REQUIRE(std::get<std::int64_t>(guarded.binds[0]) == 7);
    REQUIRE(std::get<std::string>(guarded.binds[1]) == "eu");
    REQUIRE(std::get<std::int64_t>(guarded.binds[2]) == 4);
}

TEST_CASE("upsert conflicts on every composite key column", "[statement][primary_key]")
{
    const composite_region row{.id = 7, .region = "eu", .title = "Renamed", .score = 3};

    const auto sqlite_model = entityfy::describe_model<composite_region>(entityfy::dialect::sqlite);
    const auto sqlite = entityfy::upsert_statement(row, sqlite_model);
    REQUIRE(sqlite.sql ==
            "INSERT INTO \"key_regions\" (\"id\", \"region\", \"title\", \"score\") VALUES (?, ?, ?, ?) "
            "ON CONFLICT (\"id\", \"region\") DO UPDATE SET "
            "\"title\" = excluded.\"title\", \"score\" = excluded.\"score\"");
    REQUIRE(sqlite.binds.size() == 4);
    REQUIRE(std::get<std::int64_t>(sqlite.binds[0]) == 7);
    REQUIRE(std::get<std::string>(sqlite.binds[1]) == "eu");
    REQUIRE(std::get<std::string>(sqlite.binds[2]) == "Renamed");
    REQUIRE(std::get<std::int64_t>(sqlite.binds[3]) == 3);

    const auto postgres_model = entityfy::describe_model<composite_region>(entityfy::dialect::postgresql);
    const auto postgres = entityfy::upsert_statement(row, postgres_model);
    REQUIRE(postgres.sql.find("ON CONFLICT (\"id\", \"region\") DO UPDATE SET") != std::string::npos);
    REQUIRE(postgres.sql == sqlite.sql);
    REQUIRE(postgres.binds == sqlite.binds);
}

TEST_CASE("partial update binds the composite key from the descriptor", "[statement][primary_key]")
{
    const auto model = entityfy::describe_model<composite_region>(entityfy::dialect::sqlite);
    const composite_region row{.id = 7, .region = "eu", .title = "Renamed", .score = 3};

    const auto partial = entityfy::update_partial_statement(row, model, &composite_region::title);
    REQUIRE(partial.sql == "UPDATE \"key_regions\" SET \"title\" = ? WHERE \"id\" = ? AND \"region\" = ?");
    REQUIRE(partial.binds.size() == 3);
    REQUIRE(std::get<std::string>(partial.binds[0]) == "Renamed");
    REQUIRE(std::get<std::int64_t>(partial.binds[1]) == 7);
    REQUIRE(std::get<std::string>(partial.binds[2]) == "eu");
}

TEST_CASE("partial update resolves annotated key column names from the descriptor", "[statement][primary_key]")
{
    const auto model = entityfy::describe_model<aliased_state>(entityfy::dialect::sqlite);
    REQUIRE(model.columns[1].name == "region_code");

    const aliased_state row{.id = 7, .region = "eu", .note = "kept"};

    const auto partial = entityfy::update_partial_statement(row, model, &aliased_state::note);
    REQUIRE(partial.sql ==
            "UPDATE \"key_aliased_states\" SET \"note\" = ? WHERE \"id\" = ? AND \"region_code\" = ?");
    REQUIRE(partial.binds.size() == 3);
    REQUIRE(std::get<std::string>(partial.binds[0]) == "kept");
    REQUIRE(std::get<std::int64_t>(partial.binds[1]) == 7);
    REQUIRE(std::get<std::string>(partial.binds[2]) == "eu");
}
