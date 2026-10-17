#include "harness.hpp"

#include <entityfy/migrate/provider.hpp>

#include <filesystem>
#include <string>

entityfy_test::status test_rename_table()
{
    const auto database_path = test_workspace("entityfy_rename_table_test.db");
    std::filesystem::remove(database_path);

    auto backend = entityfy::migrate::open_sqlite_provider(database_path.string());

    backend->execute_sql("CREATE TABLE \"we\"\"ird\" (\"id\" TEXT PRIMARY KEY)");
    backend->execute_sql("INSERT INTO \"we\"\"ird\" (\"id\") VALUES ('kept')");

    entityfy::migrate::rename_table(*backend, "we\"ird", "renamed_table");

    const auto tables = backend->query("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name");
    require(tables.size() == 1, "rename leaves exactly one table");
    require(!tables.front().empty() && tables.front().front() == "renamed_table",
            "table is renamed even when the name contains a quote");

    const auto rows = backend->query("SELECT \"id\" FROM \"renamed_table\"");
    require(rows.size() == 1 && !rows.front().empty() && rows.front().front() == "kept",
            "renamed table keeps its rows");

    backend->close();
    std::filesystem::remove(database_path);

    return entityfy_test::status::passed;
}
