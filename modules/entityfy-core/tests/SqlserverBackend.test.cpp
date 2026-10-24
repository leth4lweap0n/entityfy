#include <catch2/catch_test_macros.hpp>

#include <entityfy/detail/sqlserver_uri.hpp>
#include <entityfy/entityfy.hpp>

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
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

// No identity column: the MERGE path must not claim a generated key.
struct [[= entityfy::table{"tsql_codes"}]] tsql_code
{
    [[= entityfy::id]]
    std::int64_t code = 0;

    [[= entityfy::varchar{40}]]
    std::string label;
};

[[nodiscard]] std::string sqlserver_uri()
{
    const char* value = std::getenv("ENTITYFY_SQLSERVER_URI");
    return value == nullptr ? std::string{} : std::string{value};
}

// The URI with a query option appended, so a run can opt into MARS without
// replacing the CI-provided URI.
[[nodiscard]] std::string sqlserver_uri_with(std::string_view option)
{
    auto uri = sqlserver_uri();
    if(uri.empty())
    {
        return uri;
    }

    uri.push_back(uri.find('?') == std::string::npos ? '?' : '&');
    uri.append(option);
    return uri;
}

} // namespace

TEST_CASE("sql server URIs accept both schemes", "[sqlserver][uri]")
{
    using entityfy::detail::parse_sqlserver_uri;

    const auto sqlserver = parse_sqlserver_uri("sqlserver://sa:secret@db.local:1444/app");
    REQUIRE(sqlserver.host == "db.local");
    REQUIRE(sqlserver.port == 1444);
    REQUIRE(sqlserver.user == "sa");
    REQUIRE(sqlserver.password == "secret");
    REQUIRE(sqlserver.database == "app");
    REQUIRE(sqlserver.encrypt);
    REQUIRE(sqlserver.trust_server_certificate);
    REQUIRE_FALSE(sqlserver.mars);

    // mssql:// is the same scheme; the dispatcher in entityfy.cpp accepts it.
    const auto mssql = parse_sqlserver_uri("mssql://sa:secret@db.local:1444/app");
    REQUIRE(mssql.host == sqlserver.host);
    REQUIRE(mssql.port == sqlserver.port);
    REQUIRE(mssql.password == sqlserver.password);
    REQUIRE(mssql.database == sqlserver.database);

    // The defaults only fill what the URI omits.
    const auto minimal = parse_sqlserver_uri("sqlserver:///master");
    REQUIRE(minimal.host == "127.0.0.1");
    REQUIRE(minimal.port == 1433);
    REQUIRE(minimal.user == "sa");
    REQUIRE(minimal.password.empty());
    REQUIRE(minimal.database == "master");

    REQUIRE_THROWS_AS(parse_sqlserver_uri("postgres://sa:x@db:5432/app"), std::invalid_argument);
}

TEST_CASE("sql server URIs percent-decode credentials", "[sqlserver][uri]")
{
    using entityfy::detail::parse_sqlserver_uri;

    const auto options = parse_sqlserver_uri("mssql://sa:p%40ss%3Bword@db:1433/app");
    REQUIRE(options.password == "p@ss;word");

    // The decoded credentials must not be re-split on the URI separators.
    const auto connection_string = entityfy::detail::sqlserver_connection_string(options);
    REQUIRE(connection_string.find("PWD={p@ss;word}") != std::string::npos);

    const auto database = parse_sqlserver_uri("sqlserver://sa:x@db:1433/my%20database");
    REQUIRE(database.database == "my database");
    REQUIRE(entityfy::detail::sqlserver_connection_string(database).find("DATABASE={my database}") !=
            std::string::npos);

    // A stray percent sign is data, not a broken escape.
    const auto literal = parse_sqlserver_uri("sqlserver://sa:pa%ss@db:1433/app");
    REQUIRE(literal.password == "pa%ss");
}

TEST_CASE("sql server URIs require a database", "[sqlserver][uri]")
{
    using entityfy::detail::parse_sqlserver_uri;

    REQUIRE_THROWS_AS(parse_sqlserver_uri("sqlserver://sa:secret@db:1433"), std::invalid_argument);
    REQUIRE_THROWS_AS(parse_sqlserver_uri("sqlserver://sa:secret@db:1433/"), std::invalid_argument);
    REQUIRE_THROWS_AS(parse_sqlserver_uri("mssql://db:1433"), std::invalid_argument);
}

TEST_CASE("sql server connection strings brace credentials", "[sqlserver][uri]")
{
    using entityfy::detail::sqlserver_connection_string;
    using entityfy::detail::sqlserver_uri_options;

    sqlserver_uri_options options;
    options.host = "db.local";
    options.port = 1444;
    options.user = "sa";
    // A ';' would end UID and a '}' would end the braced value without the
    // doubling rule, so this one password covers both escapes.
    options.password = "p;{a}b";
    options.database = "app";

    const auto connection_string = sqlserver_connection_string(options);
    REQUIRE(connection_string.find("SERVER=db.local,1444;") != std::string::npos);
    REQUIRE(connection_string.find("UID={sa};") != std::string::npos);
    REQUIRE(connection_string.find("PWD={p;{a}}b};") != std::string::npos);
    REQUIRE(connection_string.find("DATABASE={app};") != std::string::npos);
    REQUIRE(connection_string.find("Encrypt=yes") != std::string::npos);
    REQUIRE(connection_string.find("TrustServerCertificate=yes") != std::string::npos);

    // The braced password stays a single value: the ';' inside it does not
    // start a new keyword.
    const auto password_begin = connection_string.find("PWD=");
    const auto password_end = connection_string.find(";DATABASE=", password_begin);
    REQUIRE(connection_string.substr(password_begin, password_end - password_begin) == "PWD={p;{a}}b}");

    options.encrypt = false;
    options.trust_server_certificate = false;
    const auto insecure = sqlserver_connection_string(options);
    REQUIRE(insecure.find("Encrypt=no") != std::string::npos);
    REQUIRE(insecure.find("TrustServerCertificate=no") != std::string::npos);
}

TEST_CASE("sql server URIs parse the query options", "[sqlserver][uri]")
{
    using entityfy::detail::parse_sqlserver_uri;

    const auto mars = parse_sqlserver_uri("sqlserver://sa:x@db:1433/app?mars=1");
    REQUIRE(mars.mars);
    REQUIRE(entityfy::detail::sqlserver_connection_string(mars).find("MARS_Connection=yes") !=
            std::string::npos);

    REQUIRE(parse_sqlserver_uri("sqlserver://sa:x@db:1433/app?mars=true").mars);
    REQUIRE_FALSE(parse_sqlserver_uri("sqlserver://sa:x@db:1433/app?mars=0").mars);
    REQUIRE_FALSE(parse_sqlserver_uri("sqlserver://sa:x@db:1433/app?mars=false").mars);

    const auto insecure = parse_sqlserver_uri("sqlserver://sa:x@db:1433/app?encrypt=0&trust_server_certificate=0");
    REQUIRE_FALSE(insecure.encrypt);
    REQUIRE_FALSE(insecure.trust_server_certificate);

    const auto strict = parse_sqlserver_uri("sqlserver://sa:x@db:1433/app?encrypt=1&trust_server_certificate=1");
    REQUIRE(strict.encrypt);
    REQUIRE(strict.trust_server_certificate);

    // An unknown option is ignored so a newer URI still connects.
    REQUIRE_NOTHROW(parse_sqlserver_uri("sqlserver://sa:x@db:1433/app?application_name=entityfy"));
    REQUIRE_THROWS_AS(parse_sqlserver_uri("sqlserver://sa:x@db:1433/app?mars=maybe"), std::invalid_argument);
}

TEST_CASE("sql server native errors map onto typed failures", "[sqlserver][error]")
{
    using entityfy::detail::classify_sqlserver_native;
    using entityfy::detail::sqlserver_error_kind;

    REQUIRE(classify_sqlserver_native(1205) == sqlserver_error_kind::deadlock);

    REQUIRE(classify_sqlserver_native(1222) == sqlserver_error_kind::transient);
    REQUIRE(classify_sqlserver_native(40613) == sqlserver_error_kind::transient);
    REQUIRE(classify_sqlserver_native(40197) == sqlserver_error_kind::transient);
    REQUIRE(classify_sqlserver_native(40501) == sqlserver_error_kind::transient);
    REQUIRE(classify_sqlserver_native(49918) == sqlserver_error_kind::transient);
    REQUIRE(classify_sqlserver_native(49919) == sqlserver_error_kind::transient);
    REQUIRE(classify_sqlserver_native(49920) == sqlserver_error_kind::transient);
    REQUIRE(classify_sqlserver_native(10928) == sqlserver_error_kind::transient);
    REQUIRE(classify_sqlserver_native(10929) == sqlserver_error_kind::transient);
    REQUIRE(classify_sqlserver_native(10053) == sqlserver_error_kind::transient);
    REQUIRE(classify_sqlserver_native(10054) == sqlserver_error_kind::transient);
    REQUIRE(classify_sqlserver_native(10060) == sqlserver_error_kind::transient);

    REQUIRE(classify_sqlserver_native(2601) == sqlserver_error_kind::duplicate_key);
    REQUIRE(classify_sqlserver_native(2627) == sqlserver_error_kind::duplicate_key);
    REQUIRE(classify_sqlserver_native(547) == sqlserver_error_kind::foreign_key);
    REQUIRE(classify_sqlserver_native(515) == sqlserver_error_kind::not_null);
    REQUIRE(classify_sqlserver_native(18456) == sqlserver_error_kind::authentication);
    REQUIRE(classify_sqlserver_native(4060) == sqlserver_error_kind::connection);

    // An unrelated number keeps the generic path.
    REQUIRE(classify_sqlserver_native(0) == sqlserver_error_kind::none);
    REQUIRE(classify_sqlserver_native(50000) == sqlserver_error_kind::none);
}

TEST_CASE("sql server statements use the T-SQL dialect", "[sqlserver][statement]")
{
    const auto model = entityfy::describe_model<tsql_user>(entityfy::dialect::sqlserver);
    REQUIRE(model.columns[0].sql_type == "BIGINT");
    REQUIRE(model.columns[1].sql_type == "NVARCHAR(120)");

    const auto table = entityfy::create_table(model);
    REQUIRE(table.sql.find("IDENTITY(1,1)") != std::string::npos);

    const auto insert = entityfy::insert_statement(tsql_user{}, model);
    REQUIRE(insert.sql.find("OUTPUT INSERTED.\"id\"") != std::string::npos);
    REQUIRE(insert.sql.find("OUTPUT INSERTED.\"id\" OUTPUT INSERTED") == std::string::npos);

    const auto upsert = entityfy::upsert_statement(tsql_user{}, model);
    REQUIRE(upsert.sql.find("MERGE INTO") != std::string::npos);
    REQUIRE(upsert.sql.find("WHEN MATCHED THEN UPDATE SET") != std::string::npos);
    REQUIRE(upsert.sql.find("WHEN NOT MATCHED THEN INSERT") != std::string::npos);

    // Exactly one OUTPUT, on the INSERT clause, before the terminator.
    const auto output = upsert.sql.find("OUTPUT INSERTED.\"id\"");
    REQUIRE(output != std::string::npos);
    REQUIRE(upsert.sql.find("OUTPUT INSERTED", output + 1) == std::string::npos);
    REQUIRE(upsert.sql.back() == ';');
    REQUIRE(output < upsert.sql.size() - 1);
    REQUIRE(upsert.sql.find(") OUTPUT INSERTED.\"id\";") != std::string::npos);

    // The OUTPUT sits inside the WHEN NOT MATCHED clause, not before it.
    REQUIRE(upsert.sql.find("WHEN NOT MATCHED THEN INSERT") < output);

    // A model without an identity column has nothing generated to return.
    const auto code_model = entityfy::describe_model<tsql_code>(entityfy::dialect::sqlserver);
    const auto code_upsert = entityfy::upsert_statement(tsql_code{}, code_model);
    REQUIRE(code_upsert.sql.find("OUTPUT INSERTED") == std::string::npos);

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

TEST_CASE("sql server upsert returns the generated key", "[sqlserver][backend]")
{
    const auto uri = sqlserver_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_SQLSERVER_URI is not set");
    }

    entityfy::client db{uri};
    db.execute(entityfy::statement{.sql = "IF OBJECT_ID('dbo.tsql_upsert_keys') IS NOT NULL DROP TABLE dbo.tsql_upsert_keys"});
    db.execute(entityfy::statement{.sql =
                                       "CREATE TABLE \"tsql_upsert_keys\" (\"id\" BIGINT IDENTITY(1,1) PRIMARY KEY, "
                                       "\"label\" NVARCHAR(40) NOT NULL)"});

    struct [[= entityfy::table{"tsql_upsert_keys"}]] upsert_row
    {
        [[= entityfy::id, = entityfy::auto_increment]]
        std::int64_t id = 0;

        [[= entityfy::varchar{40}, = entityfy::not_null]]
        std::string label;
    };

    // The insert branch of the MERGE generates the key.
    const auto inserted = db.upsert(upsert_row{.label = "first"});
    REQUIRE(inserted.last_insert_id > 0);

    // The update branch returns the existing key of the matched row.
    auto changed = upsert_row{.id = inserted.last_insert_id, .label = "renamed"};
    const auto updated = db.upsert(changed);
    REQUIRE(updated.last_insert_id == inserted.last_insert_id);
    REQUIRE(db.count<upsert_row>() == 1);
    REQUIRE(db.find<upsert_row>(inserted.last_insert_id)->label == "renamed");

    db.execute(entityfy::statement{.sql = "DROP TABLE dbo.tsql_upsert_keys"});
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

TEST_CASE("sql server transaction isolation and savepoints", "[sqlserver][transaction]")
{
    const auto uri = sqlserver_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_SQLSERVER_URI is not set");
    }

    entityfy::client db{uri};
    REQUIRE(db.transaction_isolation() == "READ COMMITTED");

    db.set_transaction_isolation("serializable");
    REQUIRE(db.transaction_isolation() == "SERIALIZABLE");
    REQUIRE_THROWS_AS(db.set_transaction_isolation("read dirty"), std::invalid_argument);

    db.execute(entityfy::statement{.sql = "IF OBJECT_ID('dbo.tsql_isolation') IS NOT NULL DROP TABLE dbo.tsql_isolation"});
    db.execute(entityfy::statement{.sql =
                                       "CREATE TABLE \"tsql_isolation\" (\"id\" INTEGER PRIMARY KEY, "
                                       "\"name\" NVARCHAR(40) NOT NULL)"});

    const auto level = [&]() -> std::string {
        const auto rows = db.query(
            entityfy::statement{.sql = "SELECT CAST(CASE transaction_isolation_level "
                                       "WHEN 1 THEN 'READ UNCOMMITTED' "
                                       "WHEN 2 THEN 'READ COMMITTED' "
                                       "WHEN 3 THEN 'REPEATABLE READ' "
                                       "WHEN 4 THEN 'SERIALIZABLE' "
                                       "WHEN 5 THEN 'SNAPSHOT' END AS NVARCHAR(20)) "
                                       "FROM sys.dm_exec_sessions WHERE session_id = @@SPID"});
        return std::get<std::string>(rows.front().front());
    };

    db.transaction([&](entityfy::client& tx) {
        REQUIRE(level() == "SERIALIZABLE");
        tx.execute(entityfy::statement{.sql = "INSERT INTO \"tsql_isolation\" (\"id\", \"name\") VALUES (1, 'outer')"});

        // A nested transaction is a savepoint: the level is not re-emitted, and
        // rolling back to the savepoint keeps the outer row.
        tx.transaction([&](entityfy::client& inner) {
            inner.execute(
                entityfy::statement{.sql = "INSERT INTO \"tsql_isolation\" (\"id\", \"name\") VALUES (2, 'inner')"});
        });

        return 0;
    });

    const auto rows = db.query(entityfy::statement{.sql = "SELECT \"id\" FROM \"tsql_isolation\" ORDER BY \"id\""});
    REQUIRE(rows.size() == 2);

    db.execute(entityfy::statement{.sql = "DROP TABLE dbo.tsql_isolation"});
}

TEST_CASE("sql server closes a cursor after an early query_each exit", "[sqlserver][mars]")
{
    const auto uri = sqlserver_uri_with("mars=1");
    if(uri.empty())
    {
        SKIP("ENTITYFY_SQLSERVER_URI is not set");
    }

    entityfy::client db{uri};

    // MARS lets a second statement run while the first one's cursor is open;
    // with the cursor closed on the early exit, a different statement can even
    // reuse the connection without draining the first result set.
    std::size_t seen = 0;
    db.query_each(
        entityfy::statement{.sql = "SELECT TOP (100) object_id FROM sys.objects ORDER BY object_id"},
        [&](const entityfy::detail::row&) {
            ++seen;
            return false;
        }
    );
    REQUIRE(seen == 1);

    const auto other = db.query(entityfy::statement{.sql = "SELECT CAST(42 AS BIGINT)"});
    REQUIRE(other.size() == 1);
    REQUIRE(std::get<std::int64_t>(other.front().front()) == 42);

    // The cached handle for the first query is reusable: an early exit on a
    // second execution still leaves a working statement behind.
    seen = 0;
    db.query_each(
        entityfy::statement{.sql = "SELECT TOP (100) object_id FROM sys.objects ORDER BY object_id"},
        [&](const entityfy::detail::row&) {
            ++seen;
            return seen < 2;
        }
    );
    REQUIRE(seen == 2);

    const auto count = db.query(entityfy::statement{.sql = "SELECT CAST(COUNT(*) AS BIGINT) FROM sys.objects"});
    REQUIRE(std::get<std::int64_t>(count.front().front()) > 0);
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
