#include <catch2/catch_test_macros.hpp>

#include <entityfy/entityfy.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

namespace
{

struct [[= entityfy::table{"tsql_users"}]] tsql_user
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::unique, = entityfy::varchar{120}, = entityfy::not_null]]
    std::string email;

    [[= entityfy::varchar{80}]]
    std::string name;
};

[[nodiscard]] std::string sqlserver_uri()
{
    const char* value = std::getenv("ENTITYFY_SQLSERVER_URI");
    return value == nullptr ? std::string{} : std::string{value};
}

} // namespace

TEST_CASE("sql server statements use the T-SQL dialect", "[sqlserver][statement]")
{
    const auto model = entityfy::describe_model<tsql_user>(entityfy::dialect::sqlserver);
    REQUIRE(model.columns[0].sql_type == "BIGINT");
    REQUIRE(model.columns[1].sql_type == "NVARCHAR(120)");

    const auto table = entityfy::create_table(model);
    REQUIRE(table.sql.find("IDENTITY(1,1)") != std::string::npos);

    const auto insert = entityfy::insert_statement(tsql_user{}, model);
    REQUIRE(insert.sql.find("OUTPUT INSERTED.\"id\"") != std::string::npos);

    const auto upsert = entityfy::upsert_statement(tsql_user{}, model);
    REQUIRE(upsert.sql.find("MERGE INTO") != std::string::npos);
    REQUIRE(upsert.sql.find("WHEN MATCHED THEN UPDATE SET") != std::string::npos);
    REQUIRE(upsert.sql.find("WHEN NOT MATCHED THEN INSERT") != std::string::npos);

    entityfy::query_options<tsql_user> options;
    options.limit = 5;
    options.offset = 10;
    const auto select = entityfy::select_statement(model, options);
    REQUIRE(select.sql.find("ORDER BY (SELECT NULL) OFFSET 10 ROWS FETCH NEXT 5 ROWS ONLY") != std::string::npos);

    const auto indexes = entityfy::create_index_statements(model);
    REQUIRE_FALSE(indexes.empty());
    for(const auto& index : indexes)
    {
        REQUIRE(index.sql.find("IF NOT EXISTS") == std::string::npos);
    }
}

TEST_CASE("sql server backend round trip", "[sqlserver][backend]")
{
    const auto uri = sqlserver_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_SQLSERVER_URI is not set");
    }

    entityfy::client db{uri};
    {
        const auto check = db.query(entityfy::statement{
            .sql = "SELECT CAST(SESSIONPROPERTY('QUOTED_IDENTIFIER') AS NVARCHAR(4))"});
        REQUIRE(std::get<std::string>(check.front().front()) == "1");
    }
    db.execute(entityfy::statement{.sql = "IF OBJECT_ID('dbo.tsql_users') IS NOT NULL DROP TABLE dbo.tsql_users"});
    db.migrate<tsql_user>();

    const auto inserted = db.insert<tsql_user>({.email = "ada@tsql.test", .name = "Ada"});
    REQUIRE(inserted.rows_affected == 1);
    REQUIRE(inserted.last_insert_id > 0);

    const auto loaded = db.find<tsql_user>(inserted.last_insert_id);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->email == "ada@tsql.test");
    REQUIRE(loaded->name == "Ada");

    auto changed = *loaded;
    changed.name = "Ada Lovelace";
    REQUIRE(db.update(changed) == 1);
    REQUIRE(db.find<tsql_user>(changed.id)->name == "Ada Lovelace");

    REQUIRE_THROWS_AS(db.insert<tsql_user>({.email = "ada@tsql.test", .name = "Duplicate"}),
                      entityfy::unique_violation);

    tsql_user second{.email = "grace@tsql.test", .name = "Grace"};
    REQUIRE(db.insert(second).last_insert_id > 0);
    REQUIRE(db.count<tsql_user>() == 2);

    const auto rows = db.find_many<tsql_user>(entityfy::query<tsql_user>().order_by(&tsql_user::id));
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[1].email == "grace@tsql.test");

    changed.name = "Ada L.";
    REQUIRE(db.upsert(changed).rows_affected >= 1);
    REQUIRE(db.count<tsql_user>() == 2);

    const auto first = db.find_many<tsql_user>(entityfy::query<tsql_user>().order_by(&tsql_user::id).take(1));
    REQUIRE(first.size() == 1);
    REQUIRE(first.front().email == "ada@tsql.test");

    REQUIRE(db.remove_by_id<tsql_user>(inserted.last_insert_id) == 1);
    REQUIRE(db.count<tsql_user>() == 1);

    db.execute(entityfy::statement{.sql = "DROP TABLE dbo.tsql_users"});
}

TEST_CASE("sql server statement cache reuses compiled statements", "[sqlserver][cache]")
{
    const auto uri = sqlserver_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_SQLSERVER_URI is not set");
    }

    entityfy::client db{uri};
    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"sqlserver_cache_items\""});
    db.execute(entityfy::statement{.sql =
                                       "CREATE TABLE \"sqlserver_cache_items\" (\"id\" INTEGER PRIMARY KEY, "
                                       "\"name\" NVARCHAR(40) NOT NULL)"});
    db.clear_statement_cache();

    REQUIRE(db.statement_cache_capacity() > 0);
    REQUIRE(db.statement_cache_size() == 0);

    const std::string insert_sql =
        "INSERT INTO \"sqlserver_cache_items\" (\"id\", \"name\") VALUES (?, ?)";

    db.execute(entityfy::statement{.sql = insert_sql, .binds = {1, std::string{"first"}}});
    REQUIRE(db.statement_cache_size() == 1);

    db.execute(entityfy::statement{.sql = insert_sql, .binds = {2, std::string{"second"}}});
    REQUIRE(db.statement_cache_size() == 1);

    const auto rows = db.query(
        entityfy::statement{.sql = "SELECT \"id\", \"name\" FROM \"sqlserver_cache_items\" ORDER BY \"id\""});
    REQUIRE(rows.size() == 2);
    REQUIRE(std::get<std::int64_t>(rows[0][0]) == 1);
    REQUIRE(std::get<std::string>(rows[1][1]) == "second");
    REQUIRE(db.statement_cache_size() == 2);

    db.set_statement_cache_capacity(1);
    REQUIRE(db.statement_cache_size() == 1);

    db.set_statement_cache_capacity(0);
    REQUIRE(db.statement_cache_size() == 0);

    const auto again = db.query(
        entityfy::statement{.sql = "SELECT \"id\" FROM \"sqlserver_cache_items\" ORDER BY \"id\""});
    REQUIRE(again.size() == 2);

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"sqlserver_cache_items\""});
}
