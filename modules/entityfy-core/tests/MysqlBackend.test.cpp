#include <catch2/catch_test_macros.hpp>

#include <entityfy/entityfy.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

namespace
{

struct [[= entityfy::table{"mysql_users"}]] mysql_user
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::unique, = entityfy::varchar{120}, = entityfy::not_null]]
    std::string email;

    [[= entityfy::varchar{80}]]
    std::string name;
};

[[nodiscard]] std::string mysql_uri()
{
    const char* value = std::getenv("ENTITYFY_MYSQL_URI");
    return value == nullptr ? std::string{} : std::string{value};
}

} // namespace

TEST_CASE("mysql statements use the MySQL dialect", "[mysql][statement]")
{
    const auto model = entityfy::describe_model<mysql_user>(entityfy::dialect::mysql);
    REQUIRE(model.columns[0].sql_type == "BIGINT");
    REQUIRE(model.columns[1].sql_type == "VARCHAR(120)");

    const auto table = entityfy::create_table(model);
    REQUIRE(table.sql.find("AUTO_INCREMENT") != std::string::npos);

    const auto upsert = entityfy::upsert_statement(mysql_user{}, model);
    REQUIRE(upsert.sql.find("ON DUPLICATE KEY UPDATE") != std::string::npos);
    REQUIRE(upsert.sql.find("VALUES(\"email\")") != std::string::npos);
    REQUIRE(upsert.sql.find("ON CONFLICT") == std::string::npos);

    const auto indexes = entityfy::create_index_statements(model);
    REQUIRE_FALSE(indexes.empty());
    for(const auto& index : indexes)
    {
        REQUIRE(index.sql.find("IF NOT EXISTS") == std::string::npos);
    }
}

TEST_CASE("mysql backend round trip", "[mysql][backend]")
{
    const auto uri = mysql_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_MYSQL_URI is not set");
    }

    entityfy::client db{uri};
    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"mysql_users\""});
    db.migrate<mysql_user>();

    const auto inserted = db.insert<mysql_user>({.email = "ada@mysql.test", .name = "Ada"});
    REQUIRE(inserted.rows_affected == 1);
    REQUIRE(inserted.last_insert_id > 0);

    const auto loaded = db.find<mysql_user>(inserted.last_insert_id);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->email == "ada@mysql.test");
    REQUIRE(loaded->name == "Ada");

    auto changed = *loaded;
    changed.name = "Ada Lovelace";
    REQUIRE(db.update(changed) == 1);
    REQUIRE(db.find<mysql_user>(changed.id)->name == "Ada Lovelace");

    REQUIRE_THROWS_AS(db.insert<mysql_user>({.email = "ada@mysql.test", .name = "Duplicate"}), entityfy::unique_violation);

    auto second = mysql_user{.email = "grace@mysql.test", .name = "Grace"};
    REQUIRE(db.insert(second).last_insert_id > 0);
    REQUIRE(db.count<mysql_user>() == 2);

    const auto rows = db.find_many<mysql_user>(entityfy::query<mysql_user>().order_by(&mysql_user::id));
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[1].email == "grace@mysql.test");

    changed.name = "Ada L.";
    REQUIRE(db.upsert(changed).rows_affected >= 1);
    REQUIRE(db.count<mysql_user>() == 2);

    REQUIRE(db.remove_by_id<mysql_user>(inserted.last_insert_id) == 1);
    REQUIRE(db.count<mysql_user>() == 1);

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"mysql_users\""});
}

TEST_CASE("mysql statement cache reuses compiled statements", "[mysql][cache]")
{
    const auto uri = mysql_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_MYSQL_URI is not set");
    }

    entityfy::client db{uri};
    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"mysql_cache_items\""});
    db.execute(entityfy::statement{.sql =
                                       "CREATE TABLE \"mysql_cache_items\" (\"id\" INTEGER PRIMARY KEY, "
                                       "\"name\" VARCHAR(40) NOT NULL)"});
    db.clear_statement_cache();

    REQUIRE(db.statement_cache_capacity() > 0);
    REQUIRE(db.statement_cache_size() == 0);

    const std::string insert_sql =
        "INSERT INTO \"mysql_cache_items\" (\"id\", \"name\") VALUES (?, ?)";

    db.execute(entityfy::statement{.sql = insert_sql, .binds = {1, std::string{"first"}}});
    REQUIRE(db.statement_cache_size() == 1);

    db.execute(entityfy::statement{.sql = insert_sql, .binds = {2, std::string{"second"}}});
    REQUIRE(db.statement_cache_size() == 1);

    const auto rows = db.query(
        entityfy::statement{.sql = "SELECT \"id\", \"name\" FROM \"mysql_cache_items\" ORDER BY \"id\""});
    REQUIRE(rows.size() == 2);
    REQUIRE(std::get<std::int64_t>(rows[0][0]) == 1);
    REQUIRE(std::get<std::string>(rows[1][1]) == "second");
    REQUIRE(db.statement_cache_size() == 2);

    db.set_statement_cache_capacity(1);
    REQUIRE(db.statement_cache_size() == 1);

    db.set_statement_cache_capacity(0);
    REQUIRE(db.statement_cache_size() == 0);

    const auto again = db.query(
        entityfy::statement{.sql = "SELECT \"id\" FROM \"mysql_cache_items\" ORDER BY \"id\""});
    REQUIRE(again.size() == 2);

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"mysql_cache_items\""});
}
