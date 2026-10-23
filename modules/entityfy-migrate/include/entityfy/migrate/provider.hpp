#pragma once

#include <entityfy/migrate/diff.hpp>
#include <entityfy/migrate/plan.hpp>
#include <entityfy/migrate/schema.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace entityfy::migrate
{

struct provider_options
{
    bool allow_destructive = false;
    bool backup = true;
    bool dry_run = false;
};

class provider
{
public:
    virtual ~provider() = default;

    [[nodiscard]] virtual std::string name() const = 0;

    // Reads the schema from the live database (tables/columns/indexes/FKs).
    [[nodiscard]] virtual schema introspect() = 0;

    // Turns the desired - actual difference into provider-specific executable steps.
    [[nodiscard]] virtual plan make_plan(
        const schema& desired,
        const schema& actual,
        const diff_options& options) = 0;

    // Provider-specific comparison settings (e.g. Postgres does not read check constraints).
    [[nodiscard]] virtual diff_options comparison_options() const
    {
        return {};
    }

    // Transaction boundary: lock + BEGIN (including PRAGMA foreign_keys=OFF on SQLite).
    virtual void start() = 0;
    // Verification + COMMIT (including foreign_key_check and restoring the pragma on SQLite).
    virtual void finish() = 0;
    virtual void abort() = 0;

    virtual void execute(const step& value) = 0;
    virtual void execute_sql(std::string_view sql) = 0;
    virtual void execute_params(std::string_view sql, const std::vector<std::string>& params) = 0;

    [[nodiscard]] virtual std::vector<std::vector<std::string>> query(std::string_view sql) = 0;
    [[nodiscard]] virtual std::vector<std::vector<std::string>> query_params(
        std::string_view sql,
        const std::vector<std::string>& params) = 0;

    // Backup taken before a destructive operation. Throws if unsupported.
    virtual void backup(std::string_view path) = 0;

    // Returns the rebuild steps as a list of SQL statements for script generation.
    [[nodiscard]] virtual std::vector<std::string> render_rebuild(const table& desired, const table& actual) = 0;

    virtual void close() = 0;
};

// Renames the table in place (ALTER TABLE ... RENAME TO ...).
// Table names are escaped with quote_ident; names containing " are handled safely.
void rename_table(provider& backend, std::string_view from, std::string_view to);

[[nodiscard]] std::unique_ptr<provider> open_sqlite_provider(
    std::string path,
    const provider_options& options = {});

[[nodiscard]] std::unique_ptr<provider> open_postgres_provider(
    std::string conninfo,
    const provider_options& options = {});

[[nodiscard]] std::unique_ptr<provider> open_mysql_provider(
    std::string uri,
    const provider_options& options = {});

} // namespace entityfy::migrate
