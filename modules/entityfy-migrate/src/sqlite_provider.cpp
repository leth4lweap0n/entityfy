#include <entityfy/migrate/provider.hpp>
#include <entityfy/migrate/util.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace entityfy::migrate
{
namespace
{

std::string text_of(sqlite3_stmt* statement, int column)
{
    const unsigned char* text = sqlite3_column_text(statement, column);
    return text == nullptr ? std::string{} : std::string{reinterpret_cast<const char*>(text)};
}

[[noreturn]] void throw_sqlite(sqlite3* database, std::string_view context)
{
    throw std::runtime_error{"entityfy/migrate:sqlite: " + std::string{context} + ": " + sqlite3_errmsg(database)};
}

std::vector<std::vector<std::string>> run_query(
    sqlite3* database,
    std::string_view sql,
    const std::vector<std::string>* params = nullptr)
{
    sqlite3_stmt* statement = nullptr;
    const std::string text{sql};

    if(sqlite3_prepare_v2(database, text.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    {
        throw_sqlite(database, "prepare failed for `" + text + "`");
    }

    if(params != nullptr)
    {
        for(std::size_t index = 0; index < params->size(); ++index)
        {
            if(sqlite3_bind_text(statement, static_cast<int>(index + 1), (*params)[index].c_str(), -1,
                                 SQLITE_TRANSIENT) != SQLITE_OK)
            {
                sqlite3_finalize(statement);
                throw_sqlite(database, "bind failed for `" + text + "`");
            }
        }
    }

    std::vector<std::vector<std::string>> rows;
    int result = SQLITE_ROW;

    while((result = sqlite3_step(statement)) == SQLITE_ROW)
    {
        std::vector<std::string> row;
        const int columns = sqlite3_column_count(statement);
        row.reserve(static_cast<std::size_t>(columns));

        for(int column = 0; column < columns; ++column)
        {
            if(sqlite3_column_type(statement, column) == SQLITE_NULL)
            {
                row.emplace_back();
            }
            else
            {
                row.push_back(text_of(statement, column));
            }
        }

        rows.emplace_back(std::move(row));
    }

    if(result != SQLITE_DONE)
    {
        const std::string message = sqlite3_errmsg(database);
        sqlite3_finalize(statement);
        throw std::runtime_error{"entityfy/migrate:sqlite: query failed for `" + text + "`: " + message};
    }

    sqlite3_finalize(statement);
    return rows;
}

void run_sql(sqlite3* database, std::string_view sql)
{
    char* error = nullptr;
    const std::string text{sql};

    if(sqlite3_exec(database, text.c_str(), nullptr, nullptr, &error) != SQLITE_OK)
    {
        const std::string message = error == nullptr ? sqlite3_errmsg(database) : error;
        sqlite3_free(error);
        throw std::runtime_error{"entityfy/migrate:sqlite: statement failed: " + message + " | SQL: " + text};
    }
}

std::string column_definition(const column& value)
{
    std::string output = quote_ident(value.name) + " ";

    if(value.primary_key && value.auto_increment)
    {
        output += "INTEGER PRIMARY KEY AUTOINCREMENT";
        return output;
    }

    output += value.type.empty() ? "TEXT" : value.type;

    if(value.primary_key)
    {
        output += " PRIMARY KEY";
    }
    else if(value.not_null)
    {
        output += " NOT NULL";
    }

    if(!value.default_sql.empty())
    {
        output += " DEFAULT " + value.default_sql;
    }

    if(!value.check_sql.empty())
    {
        output += " CHECK (" + value.check_sql + ")";
    }

    if(value.references)
    {
        output += " REFERENCES " + quote_ident(value.references->table) + " (" +
                  quote_ident(value.references->column) + ")";

        if(!value.references->on_delete.empty())
        {
            output += " ON DELETE " + value.references->on_delete;
        }

        if(!value.references->on_update.empty())
        {
            output += " ON UPDATE " + value.references->on_update;
        }
    }

    return output;
}

std::string create_table_sql(const table& value, std::string_view table_name, bool if_not_exists)
{
    std::string output = "CREATE TABLE ";
    if(if_not_exists)
    {
        output += "IF NOT EXISTS ";
    }

    output += quote_ident(table_name);
    output += " (";

    for(std::size_t index = 0; index < value.columns.size(); ++index)
    {
        if(index != 0)
        {
            output += ", ";
        }

        output += column_definition(value.columns[index]);
    }

    output += ")";
    return output;
}

std::string create_index_sql(const table& value, const index& entry)
{
    std::string output = entry.unique ? "CREATE UNIQUE INDEX IF NOT EXISTS " : "CREATE INDEX IF NOT EXISTS ";
    output += quote_ident(entry.name);
    output += " ON " + quote_ident(value.name) + " (";

    for(std::size_t index = 0; index < entry.columns.size(); ++index)
    {
        if(index != 0)
        {
            output += ", ";
        }

        output += quote_ident(entry.columns[index]);
    }

    output += ")";
    return output;
}

class sqlite_provider final : public provider
{
public:
    sqlite_provider(std::string path, provider_options options)
        : path_{std::move(path)}, options_{options}
    {
        if(sqlite3_open_v2(path_.c_str(), &database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
        {
            const std::string message = database_ == nullptr ? "unknown error" : sqlite3_errmsg(database_);
            if(database_ != nullptr)
            {
                sqlite3_close(database_);
                database_ = nullptr;
            }

            throw std::runtime_error{"entityfy/migrate:sqlite: cannot open `" + path_ + "`: " + message};
        }

        sqlite3_busy_timeout(database_, 15000);
    }

    ~sqlite_provider() override
    {
        close();
    }

    [[nodiscard]] std::string name() const override
    {
        return "sqlite";
    }

    [[nodiscard]] schema introspect() override
    {
        schema output;

        const auto tables = run_query(
            database_,
            "SELECT name, sql FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
            "AND name <> 'entityfy_schema_migrations' ORDER BY name");

        for(const auto& table_row: tables)
        {
            if(table_row.size() < 2)
            {
                continue;
            }

            table current;
            current.name = table_row[0];
            const std::string table_sql = table_row[1];
            const bool table_has_autoincrement = table_sql.find("AUTOINCREMENT") != std::string::npos;

            const auto columns = run_query(database_, "PRAGMA table_xinfo(" + quote_ident(current.name) + ")");
            for(const auto& column_row: columns)
            {
                if(column_row.size() < 7 || column_row[6] != "0")
                {
                    continue;
                }

                column entry;
                entry.name = column_row[1];
                entry.type = column_row[2];
                entry.not_null = column_row[3] == "1";
                entry.default_sql = column_row[4];
                entry.primary_key = column_row[5] != "0" && !column_row[5].empty();
                entry.not_null = entry.not_null || entry.primary_key;
                entry.auto_increment = entry.primary_key && table_has_autoincrement;
                current.columns.emplace_back(std::move(entry));
            }

            const auto indexes = run_query(database_, "PRAGMA index_list(" + quote_ident(current.name) + ")");
            for(const auto& index_row: indexes)
            {
                if(index_row.size() < 4 || index_row[3] == "pk")
                {
                    continue;
                }

                if(index_row[1].rfind("sqlite_autoindex", 0) == 0)
                {
                    continue;
                }

                index entry;
                entry.name = index_row[1];
                entry.unique = index_row[2] == "1";

                const auto index_columns = run_query(database_, "PRAGMA index_info(" + quote_ident(entry.name) + ")");
                for(const auto& column_row: index_columns)
                {
                    if(column_row.size() >= 3 && !column_row[2].empty())
                    {
                        entry.columns.push_back(column_row[2]);
                    }
                }

                if(!entry.columns.empty())
                {
                    current.indexes.emplace_back(std::move(entry));
                }
            }

            const auto foreign_keys = run_query(database_, "PRAGMA foreign_key_list(" + quote_ident(current.name) + ")");
            for(const auto& key_row: foreign_keys)
            {
                if(key_row.size() < 8 || key_row[1] != "0")
                {
                    continue;
                }

                foreign_key entry;
                entry.column = key_row[3];
                entry.ref_table = key_row[2];
                entry.ref_column = key_row[4].empty() ? "id" : key_row[4];
                entry.on_update = key_row[5] == "NO ACTION" ? std::string{} : key_row[5];
                entry.on_delete = key_row[6] == "NO ACTION" ? std::string{} : key_row[6];

                current.foreign_keys.emplace_back(entry);

                if(column* target = find_column(current, entry.column))
                {
                    target->references = reference{
                        .table = entry.ref_table,
                        .column = entry.ref_column,
                        .on_delete = entry.on_delete,
                        .on_update = entry.on_update,
                    };
                }
            }

            output.tables.emplace_back(std::move(current));
        }

        sort_schema(output);
        return output;
    }

    [[nodiscard]] plan make_plan(
        const schema& desired,
        const schema& actual,
        const diff_options& options) override
    {
        plan output;
        const auto operations = diff_schemas(desired, actual, options);
        output.destructive = has_destructive(operations);

        if(operations.empty())
        {
            return output;
        }

        for(const auto& value: operations)
        {
            if(value.kind != op_kind::create_table)
            {
                continue;
            }

            const table* desired_table = find_table(desired, value.table);
            if(desired_table == nullptr)
            {
                continue;
            }

            step create;
            create.sql = create_table_sql(*desired_table, desired_table->name, true);
            create.label = "create table " + desired_table->name;
            output.steps.emplace_back(std::move(create));

            for(const auto& entry: desired_table->indexes)
            {
                step index_step;
                index_step.sql = create_index_sql(*desired_table, entry);
                output.steps.emplace_back(std::move(index_step));
            }
        }

        std::map<std::string, std::vector<operation>, std::less<>> by_table;
        for(const auto& value: operations)
        {
            if(value.kind == op_kind::create_table || value.kind == op_kind::drop_table)
            {
                continue;
            }

            by_table[value.table].push_back(value);
        }

        for(const auto& desired_table: desired.tables)
        {
            const auto found = by_table.find(desired_table.name);
            if(found == by_table.end())
            {
                continue;
            }

            const table* actual_table = find_table(actual, desired_table.name);
            if(actual_table == nullptr)
            {
                continue;
            }

            const auto& table_operations = found->second;

            const bool has_rebuild_operation = std::ranges::any_of(table_operations, [](const operation& value) {
                if(value.kind == op_kind::drop_column || value.kind == op_kind::alter_column ||
                   value.kind == op_kind::add_foreign_key || value.kind == op_kind::drop_foreign_key)
                {
                    return true;
                }

                if(value.kind == op_kind::add_column)
                {
                    return value.column_spec.not_null && value.column_spec.default_sql.empty();
                }

                return false;
            });

            table effective = *actual_table;

            for(const auto& value: table_operations)
            {
                if(value.kind != op_kind::rename_column)
                {
                    continue;
                }

                step rename;
                rename.sql = "ALTER TABLE " + quote_ident(desired_table.name) + " RENAME COLUMN " +
                             quote_ident(value.old_name) + " TO " + quote_ident(value.new_name);
                rename.label = value.detail;
                output.steps.emplace_back(std::move(rename));

                if(column* target = find_column(effective, value.old_name))
                {
                    target->name = value.new_name;
                }
            }

            if(has_rebuild_operation)
            {
                step rebuild;
                rebuild.type = step::kind::sqlite_rebuild;
                rebuild.desired_table = desired_table;
                rebuild.actual_table = effective;
                rebuild.label = "rebuild table " + desired_table.name;
                rebuild.sql = "-- rebuild " + desired_table.name;
                output.steps.emplace_back(std::move(rebuild));
                continue;
            }

            for(const auto& value: table_operations)
            {
                if(value.kind == op_kind::add_column)
                {
                    step add;
                    add.sql = "ALTER TABLE " + quote_ident(desired_table.name) + " ADD COLUMN " +
                              column_definition(value.column_spec);
                    add.label = value.detail;
                    output.steps.emplace_back(std::move(add));
                }
                else if(value.kind == op_kind::add_index)
                {
                    step add;
                    add.sql = create_index_sql(desired_table, value.index_spec);
                    add.label = value.detail;
                    output.steps.emplace_back(std::move(add));
                }
                else if(value.kind == op_kind::drop_index)
                {
                    step drop;
                    drop.sql = "DROP INDEX IF EXISTS " + quote_ident(value.index_spec.name);
                    drop.label = value.detail;
                    output.steps.emplace_back(std::move(drop));
                }
            }
        }

        for(const auto& table_value: actual.tables)
        {
            const bool dropped = std::ranges::any_of(operations, [&](const operation& value) {
                return value.kind == op_kind::drop_table && value.table == table_value.name;
            });

            if(dropped)
            {
                step drop;
                drop.sql = "DROP TABLE IF EXISTS " + quote_ident(table_value.name);
                drop.label = "drop table " + table_value.name;
                output.steps.emplace_back(std::move(drop));
            }
        }

        return output;
    }

    void start() override
    {
        run_sql(database_, "PRAGMA foreign_keys = OFF");

        try
        {
            run_sql(database_, "BEGIN IMMEDIATE");
        }
        catch(...)
        {
            try
            {
                run_sql(database_, "PRAGMA foreign_keys = ON");
            }
            catch(...)
            {
            }

            throw;
        }

        in_transaction_ = true;
    }

    void finish() override
    {
        const auto violations = run_query(database_, "PRAGMA foreign_key_check");
        if(!violations.empty())
        {
            const std::string table = violations.front().empty() ? "unknown" : violations.front().front();
            throw std::runtime_error{"entityfy/migrate:sqlite: foreign key check failed on table " + table};
        }

        run_sql(database_, "COMMIT");
        in_transaction_ = false;
        run_sql(database_, "PRAGMA foreign_keys = ON");
    }

    void abort() override
    {
        if(in_transaction_)
        {
            try
            {
                run_sql(database_, "ROLLBACK");
            }
            catch(...)
            {
            }

            in_transaction_ = false;
        }

        try
        {
            run_sql(database_, "PRAGMA foreign_keys = ON");
        }
        catch(...)
        {
        }
    }

    void execute(const step& value) override
    {
        if(value.type == step::kind::sql)
        {
            run_sql(database_, value.sql);
            return;
        }

        rebuild_table(value.desired_table, value.actual_table);
    }

    void execute_sql(std::string_view sql) override
    {
        run_sql(database_, sql);
    }

    void execute_params(std::string_view sql, const std::vector<std::string>& params) override
    {
        run_query(database_, sql, &params);
    }

    [[nodiscard]] std::vector<std::vector<std::string>> query(std::string_view sql) override
    {
        return run_query(database_, sql);
    }

    [[nodiscard]] std::vector<std::vector<std::string>> query_params(
        std::string_view sql,
        const std::vector<std::string>& params) override
    {
        return run_query(database_, sql, &params);
    }

    void backup(std::string_view path) override
    {
        std::error_code error;
        std::filesystem::remove(std::filesystem::path{path}, error);
        run_sql(database_, "VACUUM INTO " + quote_literal(path));
    }

    [[nodiscard]] std::vector<std::string> render_rebuild(const table& desired, const table& actual) override
    {
        return rebuild_statements(desired, actual);
    }

    void close() override
    {
        if(database_ != nullptr)
        {
            sqlite3_close(database_);
            database_ = nullptr;
        }
    }

private:
    [[nodiscard]] std::vector<std::string> rebuild_statements(const table& desired, const table& actual) const
    {
        std::vector<std::string> output;
        const std::string temporary = "__entityfy_new_" + desired.name;

        std::string saved_sequence;
        const bool wants_autoincrement = std::ranges::any_of(desired.columns, [](const column& value) {
            return value.primary_key && value.auto_increment;
        });

        if(wants_autoincrement)
        {
            try
            {
                const auto rows = run_query(
                    database_,
                    "SELECT seq FROM sqlite_sequence WHERE name = " + quote_literal(desired.name));

                if(!rows.empty() && !rows.front().empty())
                {
                    saved_sequence = rows.front().front();
                }
            }
            catch(...)
            {
                // sqlite_sequence may not exist yet.
            }
        }

        output.push_back("DROP TABLE IF EXISTS " + quote_ident(temporary));
        output.push_back(create_table_sql(desired, temporary, false));

        std::vector<std::string> common;
        for(const auto& entry: desired.columns)
        {
            if(find_column(actual, entry.name) != nullptr)
            {
                common.push_back(entry.name);
            }
        }

        if(common.empty())
        {
            std::size_t existing_rows = 0;

            try
            {
                const auto count = run_query(database_, "SELECT COUNT(*) FROM " + quote_ident(desired.name));
                if(!count.empty() && !count.front().empty())
                {
                    existing_rows = static_cast<std::size_t>(std::stoull(count.front().front()));
                }
            }
            catch(const std::exception& error)
            {
                if(std::string{error.what()}.find("no such table") == std::string::npos)
                {
                    throw;
                }
            }

            if(existing_rows != 0)
            {
                throw std::runtime_error{"entityfy/migrate:sqlite: rebuild of " + desired.name +
                                         " has no common columns with the existing data; refusing to lose rows"};
            }
        }
        else
        {
            std::string columns;
            for(std::size_t index = 0; index < common.size(); ++index)
            {
                if(index != 0)
                {
                    columns += ", ";
                }

                columns += quote_ident(common[index]);
            }

            output.push_back("INSERT INTO " + quote_ident(temporary) + " (" + columns + ") SELECT " + columns +
                             " FROM " + quote_ident(desired.name));
        }

        const auto triggers = run_query(
            database_,
            "SELECT sql FROM sqlite_master WHERE type = 'trigger' AND tbl_name = " +
                quote_literal(desired.name) + " AND sql IS NOT NULL");

        output.push_back("DROP TABLE " + quote_ident(desired.name));
        output.push_back("ALTER TABLE " + quote_ident(temporary) + " RENAME TO " + quote_ident(desired.name));

        for(const auto& entry: desired.indexes)
        {
            output.push_back(create_index_sql(desired, entry));
        }

        for(const auto& trigger: triggers)
        {
            if(!trigger.empty() && !trigger.front().empty())
            {
                output.push_back(trigger.front());
            }
        }

        if(wants_autoincrement && !saved_sequence.empty())
        {
            output.push_back("UPDATE sqlite_sequence SET seq = " + saved_sequence + " WHERE name = " +
                             quote_literal(desired.name));
            output.push_back("INSERT INTO sqlite_sequence(name, seq) SELECT " + quote_literal(desired.name) + ", " +
                             saved_sequence + " WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name = " +
                             quote_literal(desired.name) + ")");
        }

        return output;
    }

    void rebuild_table(const table& desired, const table& actual)
    {
        for(const auto& sql: rebuild_statements(desired, actual))
        {
            run_sql(database_, sql);
        }
    }

    std::string path_;
    provider_options options_;
    sqlite3* database_ = nullptr;
    bool in_transaction_ = false;
};

} // namespace

std::unique_ptr<provider> open_sqlite_provider(std::string path, const provider_options& options)
{
    constexpr std::string_view prefix = "sqlite://";
    if(path.starts_with(prefix))
    {
        path = path.substr(prefix.size());
    }

    if(path.empty())
    {
        throw std::invalid_argument{"entityfy/migrate:sqlite: database path is empty"};
    }

    return std::make_unique<sqlite_provider>(std::move(path), options);
}

} // namespace entityfy::migrate
