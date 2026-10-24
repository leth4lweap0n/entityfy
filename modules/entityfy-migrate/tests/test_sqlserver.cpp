#include "harness.hpp"

#include <entityfy/detail/sqlserver_uri.hpp>
#include <entityfy/migrate/engine.hpp>
#include <entityfy/migrate/provider.hpp>

#include "models.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

// These run without a server: they pin the wiring this provider depends on,
// namely that the migrate provider consumes the core URI grammar (so mssql://
// and percent-decoding behave like the runtime client) and that a semicolon in
// a password stays inside one braced ODBC value instead of adding a keyword.
void require_uri_wiring()
{
    using entityfy::detail::parse_sqlserver_uri;
    using entityfy::detail::sqlserver_connection_string;

    // An mssql:// URI with a percent-encoded semicolon in the password: the
    // alias must resolve and the decoded ';' must reach ODBC as one value.
    const auto options = parse_sqlserver_uri("mssql://sa:p%3Bword@db.local:1444/app");
    require(options.host == "db.local", "sqlserver migrate accepts the mssql:// alias host");
    require(options.port == 1444, "sqlserver migrate accepts the mssql:// alias port");
    require(options.database == "app", "sqlserver migrate accepts the mssql:// alias database");
    require(options.password == "p;word", "sqlserver migrate percent-decodes the password");
    require(!options.mars, "sqlserver migrate leaves MARS off unless the URI asks for it");
    require(options.encrypt && options.trust_server_certificate, "sqlserver migrate keeps the core encryption defaults");

    const auto text = sqlserver_connection_string(options);
    const auto password_begin = text.find("PWD=");
    const auto password_end = text.find(";DATABASE=", password_begin);
    require(password_begin != std::string::npos && password_end != std::string::npos,
            "sqlserver migrate writes PWD and DATABASE keywords");
    require(text.substr(password_begin, password_end - password_begin) == "PWD={p;word}",
            "sqlserver migrate keeps a ';' password inside one braced ODBC value");

    // '}' closes a braced value early unless it is doubled.
    auto closing_brace = options;
    closing_brace.password = "a}b";
    const auto braced = sqlserver_connection_string(closing_brace);
    require(braced.find("PWD={a}}b};") != std::string::npos, "sqlserver migrate doubles '}' in the password");

    // The options the provider hands to the driver are the core ones, so the
    // query string is honoured rather than ignored.
    const auto tuned = parse_sqlserver_uri("mssql://sa:x@db:1433/app?mars=1&encrypt=0&trust_server_certificate=0");
    require(tuned.mars, "sqlserver migrate takes MARS from ?mars=1");
    const auto tuned_text = sqlserver_connection_string(tuned);
    require(tuned_text.find("Encrypt=no") != std::string::npos, "sqlserver migrate applies ?encrypt=0");
    require(tuned_text.find("TrustServerCertificate=no") != std::string::npos,
            "sqlserver migrate applies ?trust_server_certificate=0");
    require(tuned_text.find("MARS_Connection=yes") != std::string::npos,
            "sqlserver migrate enables MARS through the driver connection keyword");

    // A database is never defaulted and a non-sqlserver scheme is rejected, so
    // a typo surfaces as a parse error rather than aimless connection failure.
    bool rejected_missing_database = false;
    try
    {
        (void)parse_sqlserver_uri("mssql://sa:x@db:1433");
    }
    catch(const std::invalid_argument&)
    {
        rejected_missing_database = true;
    }

    require(rejected_missing_database, "sqlserver migrate rejects a URI without a database");

    bool rejected_scheme = false;
    try
    {
        (void)parse_sqlserver_uri("postgres://sa:x@db:5432/app");
    }
    catch(const std::invalid_argument&)
    {
        rejected_scheme = true;
    }

    require(rejected_scheme, "sqlserver migrate rejects a non-sqlserver scheme");
}

// Rewrites the configured URI onto the mssql:// alias. Returns an empty string
// when the URI is already an alias, so the live alias probe only runs when it
// actually tests the alias.
[[nodiscard]] std::string alias_uri(std::string_view uri)
{
    constexpr std::string_view prefix = "sqlserver://";

    if(!uri.starts_with(prefix))
    {
        return {};
    }

    std::string alias{"mssql://"};
    alias.append(uri.substr(prefix.size()));
    return alias;
}

void cleanup(entityfy::migrate::provider& backend)
{
    const auto constraints = backend.query(
        "SELECT name, OBJECT_NAME(parent_object_id) FROM sys.foreign_keys "
        "WHERE schema_id = SCHEMA_ID()");

    for(const auto& row: constraints)
    {
        if(row.size() >= 2)
        {
            backend.execute_sql("ALTER TABLE \"" + row[1] + "\" DROP CONSTRAINT \"" + row[0] + "\"");
        }
    }

    const auto tables = backend.query("SELECT name FROM sys.tables WHERE schema_id = SCHEMA_ID()");
    for(const auto& row: tables)
    {
        if(!row.empty())
        {
            backend.execute_sql("DROP TABLE IF EXISTS \"" + row.front() + "\"");
        }
    }
}

} // namespace

entityfy_test::status test_sqlserver()
{
    // Pure checks first: they need no server and must hold even when the live
    // section below is skipped.
    require_uri_wiring();
    std::cout << "sqlserver uri wiring passed (serverless: mssql:// alias, percent-decoded braced password)\n";

    // The live alias probe is independent of the migration scenarios: it opens
    // the configured account through the mssql:// alias and asks for one
    // introspection result. A successful login proves the braced UID/PWD the
    // provider now sends is accepted by the driver end to end; the ';' handling
    // itself is pinned by require_uri_wiring above, which needs no server.
    const auto check_alias = [](std::string_view uri) {
        const auto alias = alias_uri(uri);

        if(alias.empty())
        {
            std::cout << "sqlserver alias probe skipped: ENTITYFY_SQLSERVER_URI is not a sqlserver:// URI\n";
            return;
        }

        auto backend = entityfy::migrate::open_sqlserver_provider(alias);
        auto tables = backend->query("SELECT COUNT(*) FROM INFORMATION_SCHEMA.TABLES");
        require(!tables.empty() && !tables.front().empty(), "sqlserver mssql:// alias introspection returns a row");
    };

    const char* uri = std::getenv("ENTITYFY_SQLSERVER_URI");
    if(uri == nullptr || std::string{uri}.empty())
    {
        const char* require_sqlserver = std::getenv("ENTITYFY_REQUIRE_SQLSERVER");
        const bool  required = require_sqlserver != nullptr && std::string{require_sqlserver} == "1";

        if(required)
        {
            throw std::runtime_error{"ENTITYFY_REQUIRE_SQLSERVER=1 but ENTITYFY_SQLSERVER_URI is not set"};
        }

        std::cout << "sqlserver skipped: ENTITYFY_SQLSERVER_URI is not set\n";
        return entityfy_test::status::skipped;
    }

    check_alias(uri);

    auto backend = entityfy::migrate::open_sqlserver_provider(uri);
    cleanup(*backend);

    const auto root = test_workspace("entityfy_sqlserver_test_migrations");
    std::filesystem::remove_all(root);

    const entityfy::migrate::schema initial = current_schema(entityfy::dialect::sqlserver);

    const entityfy::migrate::schema v2 = [&] {
        auto value = initial;
        entityfy::migrate::find_table(value, "users")->columns.push_back(
            entityfy::migrate::column{
                .name = "nickname",
                .type = "NVARCHAR(40)",
                .not_null = true,
                .default_sql = "'yok'",
            });
        return value;
    }();

    const entityfy::migrate::schema v3 = [&] {
        auto value = v2;
        entityfy::migrate::find_column(*entityfy::migrate::find_table(value, "users"), "name")->name = "full_name";
        return value;
    }();

    const entityfy::migrate::schema v4 = [&] {
        auto value = v3;
        entityfy::migrate::find_column(*entityfy::migrate::find_table(value, "users"), "email")->type = "NVARCHAR(100)";
        return value;
    }();

    const entityfy::migrate::schema v5 = [&] {
        auto value = v4;
        entityfy::migrate::find_table(value, "users")->columns.push_back(
            entityfy::migrate::column{.name = "extra_note", .type = "NVARCHAR(30)"});
        return value;
    }();

    std::ostringstream output;

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "sqlserver",
                                        .allow_destructive = false,
                                        .backup = false,
                                    }};

        (void)runner.add(initial, "init");
        runner.apply(initial, output);
        require(entityfy::migrate::same_schema(initial, backend->introspect()), "sqlserver initial apply matches model");

        backend->execute_sql("INSERT INTO \"users\" (\"email\", \"name\") VALUES (N'ada@test.com', N'Ada')");
        backend->execute_sql(
            "INSERT INTO \"posts\" (\"user_id\", \"title\", \"body\", \"published\") VALUES (1, N'p', N'b', 1)");

        (void)runner.add(v2, "nickname");
        runner.apply(v2, output);
        require(entityfy::migrate::same_schema(v2, backend->introspect()), "sqlserver add column applied");

        auto rows = backend->query("SELECT \"nickname\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "yok", "sqlserver default applied to existing row");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "sqlserver",
                                        .allow_destructive = false,
                                        .backup = false,
                                        .renames = {{"users.name", "full_name"}},
                                    }};

        (void)runner.add(v3, "rename_name");
        runner.apply(v3, output);
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "sqlserver rename applied");

        auto rows = backend->query("SELECT \"full_name\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "Ada", "sqlserver data preserved after rename");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "sqlserver",
                                        .allow_destructive = false,
                                        .backup = false,
                                    }};

        (void)runner.add(v4, "email_shorter");

        bool refused = false;
        try
        {
            runner.apply(v4, output);
        }
        catch(const std::exception&)
        {
            refused = true;
        }

        require(refused, "sqlserver destructive migration refused by default");
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "sqlserver database unchanged after refusal");
    }

    std::filesystem::path engine_backup_path;

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "sqlserver",
                                        .allow_destructive = true,
                                        .backup = true,
                                    }};

        runner.apply(v4, output);
        require(entityfy::migrate::same_schema(v4, backend->introspect()), "sqlserver type change applied");

        for(const auto& entry: std::filesystem::directory_iterator(root))
        {
            const auto name = entry.path().filename().string();
            if(name.starts_with("backup-") && entry.path().extension() == ".sql")
            {
                engine_backup_path = entry.path();
            }
        }

        require(!engine_backup_path.empty(), "sqlserver engine writes a .sql backup before destructive steps");

        auto rows = backend->query("SELECT \"email\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "ada@test.com", "sqlserver data preserved after type change");

        (void)runner.add(v5, "extra_note");
        runner.apply(v5, output);
        require(entityfy::migrate::same_schema(v5, backend->introspect()), "sqlserver column added with data");

        runner.rollback_last(output);
        require(entityfy::migrate::same_schema(v4, backend->introspect()), "sqlserver rollback drops the column again");

        runner.status(v4, output);
        require(output.str().find("in sync") != std::string::npos, "sqlserver status reports in sync");
    }

    backend->execute_sql("DELETE FROM \"users\" WHERE \"id\" = 1");
    const auto remaining = backend->query("SELECT COUNT(*) FROM \"posts\"");
    require(remaining.size() == 1 && remaining.front().front() == "0", "sqlserver foreign key cascade works");

    backend->execute_sql("CREATE TABLE \"backup_probe\" (\"a\" INTEGER, \"b\" NVARCHAR(20))");
    backend->execute_sql("INSERT INTO \"backup_probe\" (\"a\", \"b\") VALUES (1, NULL)");
    backend->execute_sql("INSERT INTO \"backup_probe\" (\"a\", \"b\") VALUES (2, N'it''s')");

    require(backend->backup_extension() == ".sql", "sqlserver backup uses the sql extension");

    const auto dump_path = root / "manual-backup.sql";
    backend->backup(dump_path.string());
    require(std::filesystem::exists(dump_path), "sqlserver backup file written");

    const auto dump = read_text(dump_path);
    require(dump.find("CREATE TABLE \"backup_probe\"") != std::string::npos, "sqlserver backup includes the schema");
    require(
        dump.find("INSERT INTO \"backup_probe\" (\"a\", \"b\") VALUES (1, NULL);") != std::string::npos,
        "sqlserver backup preserves NULL values"
    );
    require(dump.find("VALUES (2, N'it''s');") != std::string::npos, "sqlserver backup escapes quotes");

    cleanup(*backend);
    restore_dump(*backend, dump);

    const auto restored = backend->query("SELECT \"a\", \"b\" FROM \"backup_probe\" ORDER BY \"a\"");
    require(restored.size() == 2, "sqlserver backup restores both rows");
    require(restored.front().size() == 2 && restored.front().front() == "1", "sqlserver restored integer column");
    require(restored.back().back() == "it's", "sqlserver restored text column");

    cleanup(*backend);
    restore_dump(*backend, read_text(engine_backup_path));

    const auto backup_users = backend->query("SELECT \"full_name\" FROM \"users\"");
    require(
        backup_users.size() == 1 && backup_users.front().front() == "Ada",
        "sqlserver engine backup restores the user row"
    );

    const auto backup_posts = backend->query("SELECT COUNT(*) FROM \"posts\"");
    require(
        backup_posts.size() == 1 && backup_posts.front().front() == "1",
        "sqlserver engine backup restores the dependent row"
    );

    // Native backup: an opt-in server-side .bak, while backup() stays the
    // portable dump the engine uses for its own pre-destructive snapshots.
    require(backend->supports_native_backup(), "sqlserver advertises native backup support");

    const auto data_dir_rows = backend->query(
        "SELECT CAST(SERVERPROPERTY('InstanceDefaultDataPath') AS NVARCHAR(4000))");
    require(!data_dir_rows.empty() && !data_dir_rows.front().empty(),
            "sqlserver reports its default data directory");

    std::string data_dir = data_dir_rows.front().front();
    if(!data_dir.empty() && data_dir.back() != '\\' && data_dir.back() != '/')
    {
        data_dir.push_back('/');
    }

    const std::string server_backup_path = data_dir + "entityfy_native_backup_test.bak";
    const auto quote_literal = [](std::string_view value) {
        std::string output{value};
        std::string escaped;
        for(const char current: output)
        {
            if(current == '\'')
            {
                escaped += "''";
            }
            else
            {
                escaped.push_back(current);
            }
        }
        return escaped;
    };

    backend->backup_native(server_backup_path);
    const auto headers = backend->query("RESTORE HEADERONLY FROM DISK = N'" +
                                        quote_literal(server_backup_path) + "'");
    require(!headers.empty(), "sqlserver native backup is readable with RESTORE HEADERONLY");

    cleanup(*backend);

    return entityfy_test::status::passed;
}
