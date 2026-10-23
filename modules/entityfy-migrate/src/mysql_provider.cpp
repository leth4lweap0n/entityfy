#include <entityfy/migrate/provider.hpp>

#include <mysql.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace entityfy::migrate
{
namespace
{

std::string trim_copy(std::string_view input)
{
    const auto first = input.find_first_not_of(" \t\r\n");
    if(first == std::string_view::npos)
    {
        return {};
    }

    const auto last = input.find_last_not_of(" \t\r\n");
    return std::string{input.substr(first, last - first + 1)};
}

std::string lower_copy(std::string_view input)
{
    std::string output{input};
    for(char& character: output)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }

    return output;
}

std::string upper_copy(std::string_view input)
{
    std::string output{input};
    for(char& character: output)
    {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }

    return output;
}

std::string truncate_identifier(std::string value)
{
    if(value.size() > 64)
    {
        value.resize(64);
    }

    return value;
}

// information_schema reports MySQL display widths (`bigint(20)`), while the
// model declares plain SQL type names; map them back.
std::string normalize_mysql_type(std::string type)
{
    const auto lowered = lower_copy(trim_copy(type));
    const auto open = lowered.find('(');
    const auto base = open == std::string::npos ? lowered : lowered.substr(0, open);
    const auto parameters = open == std::string::npos ? std::string{} : lowered.substr(open);

    if(base == "tinyint" && parameters == "(1)")
    {
        return "BOOLEAN";
    }

    if(base == "tinyint" || base == "smallint" || base == "mediumint" || base == "int" || base == "integer")
    {
        return "INTEGER";
    }

    if(base == "bigint")
    {
        return "BIGINT";
    }

    if(base == "varchar" || base == "char" || base == "decimal" || base == "binary" || base == "varbinary" ||
       base == "enum" || base == "set")
    {
        return upper_copy(base) + parameters;
    }

    if(base == "text" || base == "mediumtext" || base == "longtext" || base == "tinytext" || base == "blob" ||
       base == "mediumblob" || base == "longblob" || base == "tinyblob" || base == "double" || base == "float" ||
       base == "date" || base == "datetime" || base == "timestamp" || base == "time" || base == "json" ||
       base == "uuid")
    {
        return upper_copy(base);
    }

    return upper_copy(base.empty() ? lowered : base) + parameters;
}

// information_schema returns literal defaults without quotes (`yok`) and
// expressions like `current_timestamp()`.
std::string normalize_mysql_default(std::string value)
{
    value = trim_copy(value);
    if(value.empty())
    {
        return {};
    }

    const auto lowered = lower_copy(value);

    if(lowered == "null")
    {
        return {};
    }

    if(lowered.starts_with("current_timestamp"))
    {
        return "CURRENT_TIMESTAMP";
    }

    if(lowered.starts_with("current_date"))
    {
        return "CURRENT_DATE";
    }

    if(lowered.starts_with("current_time"))
    {
        return "CURRENT_TIME";
    }

    const bool numeric = std::ranges::all_of(value, [](char character) {
        return (character >= '0' && character <= '9') || character == '-' || character == '+' || character == '.' ||
               character == 'e' || character == 'E';
    });

    if(numeric || value.front() == '\'')
    {
        return value;
    }

    std::string quoted{"'"};
    for(const char character: value)
    {
        if(character == '\'')
        {
            quoted.push_back('\'');
        }

        quoted.push_back(character);
    }

    quoted.push_back('\'');
    return quoted;
}

struct connection_options
{
    std::string host = "127.0.0.1";
    std::string user = "root";
    std::string password;
    std::string database;
    unsigned    port = 3306;
};

[[nodiscard]] connection_options parse_uri(std::string_view uri)
{
    constexpr std::string_view prefix = "mysql://";
    if(!uri.starts_with(prefix))
    {
        throw std::invalid_argument{"entityfy/migrate:mysql: connection string must start with mysql://"};
    }

    std::string_view   rest = uri.substr(prefix.size());
    connection_options options;

    if(const auto slash = rest.find('/'); slash != std::string_view::npos)
    {
        options.database = std::string{rest.substr(slash + 1)};
        rest = rest.substr(0, slash);
    }

    if(const auto at = rest.rfind('@'); at != std::string_view::npos)
    {
        const auto credentials = rest.substr(0, at);
        rest = rest.substr(at + 1);

        if(const auto colon = credentials.find(':'); colon != std::string_view::npos)
        {
            options.user = std::string{credentials.substr(0, colon)};
            options.password = std::string{credentials.substr(colon + 1)};
        }
        else
        {
            options.user = std::string{credentials};
        }
    }

    if(const auto colon = rest.rfind(':'); colon != std::string_view::npos)
    {
        options.host = std::string{rest.substr(0, colon)};
        options.port = static_cast<unsigned>(std::stoul(std::string{rest.substr(colon + 1)}));
    }
    else if(!rest.empty())
    {
        options.host = std::string{rest};
    }

    if(options.database.empty())
    {
        throw std::invalid_argument{"entityfy/migrate:mysql: URI must include a database"};
    }

    return options;
}

struct handle_deleter
{
    void operator()(MYSQL* handle) const noexcept
    {
        if(handle != nullptr)
        {
            mysql_close(handle);
        }
    }
};

using mysql_handle = std::unique_ptr<MYSQL, handle_deleter>;

[[nodiscard]] std::string error_text(MYSQL* handle)
{
    std::string message{mysql_error(handle)};
    message.append(" (errno ");
    message.append(std::to_string(mysql_errno(handle)));
    message.push_back(')');
    return message;
}

struct dump_cell
{
    bool        is_null = false;
    std::string text;
};

class mysql_connection
{
public:
    explicit mysql_connection(std::string_view uri)
    {
        static std::once_flag library_once;
        std::call_once(library_once, [] {
            mysql_library_init(0, nullptr, nullptr);
        });

        mysql_thread_init();

        const auto options = parse_uri(uri);
        handle_.reset(mysql_init(nullptr));
        if(!handle_)
        {
            throw std::runtime_error{"entityfy/migrate:mysql: client initialization failed"};
        }

        mysql_options(handle_.get(), MYSQL_SET_CHARSET_NAME, "utf8mb4");

        if(mysql_real_connect(
               handle_.get(),
               options.host.c_str(),
               options.user.c_str(),
               options.password.c_str(),
               options.database.c_str(),
               options.port,
               nullptr,
               0
           ) == nullptr)
        {
            throw std::runtime_error{"entityfy/migrate:mysql: connection failed: " + error_text(handle_.get())};
        }

        exec("SET SESSION sql_mode = CONCAT_WS(',', @@SESSION.sql_mode, 'ANSI_QUOTES', 'STRICT_ALL_TABLES', "
             "'NO_BACKSLASH_ESCAPES')");
    }

    void exec(std::string_view sql)
    {
        mysql_thread_init();
        if(mysql_real_query(handle_.get(), sql.data(), static_cast<unsigned long>(sql.size())) != 0)
        {
            throw std::runtime_error{
                "entityfy/migrate:mysql: `" + std::string{sql} + "` failed: " + error_text(handle_.get())
            };
        }
    }

    [[nodiscard]] std::vector<std::vector<std::string>> query(std::string_view sql)
    {
        return query_params(sql, {});
    }

    [[nodiscard]] std::vector<std::vector<std::string>> query_params(
        std::string_view sql,
        const std::vector<std::string>& params)
    {
        auto cells = query_cells(sql, params);

        std::vector<std::vector<std::string>> output;
        output.reserve(cells.size());

        for(auto& row: cells)
        {
            std::vector<std::string> flat;
            flat.reserve(row.size());

            for(auto& cell: row)
            {
                flat.push_back(std::move(cell.text));
            }

            output.push_back(std::move(flat));
        }

        return output;
    }

    [[nodiscard]] std::vector<std::vector<dump_cell>> query_dump(std::string_view sql)
    {
        return query_cells(sql, {});
    }

    [[nodiscard]] std::vector<std::vector<dump_cell>> query_cells(
        std::string_view sql,
        const std::vector<std::string>& params)
    {
        mysql_thread_init();

        std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)> statement{mysql_stmt_init(handle_.get()),
                                                                          mysql_stmt_close};
        if(!statement)
        {
            throw std::runtime_error{"entityfy/migrate:mysql: statement initialization failed"};
        }

        if(mysql_stmt_prepare(statement.get(), sql.data(), static_cast<unsigned long>(sql.size())) != 0)
        {
            throw std::runtime_error{"entityfy/migrate:mysql: prepare failed: " + error_text(handle_.get())};
        }

        std::vector<MYSQL_BIND>    binds(params.size());
        std::vector<unsigned long> lengths(params.size(), 0);
        std::vector<my_bool>       nulls(params.size(), 0);

        for(std::size_t index = 0; index < params.size(); ++index)
        {
            binds[index] = MYSQL_BIND{};
            binds[index].buffer_type = MYSQL_TYPE_STRING;
            binds[index].buffer = const_cast<char*>(params[index].data());
            binds[index].buffer_length = static_cast<unsigned long>(params[index].size());
            lengths[index] = binds[index].buffer_length;
            binds[index].length = &lengths[index];
            binds[index].is_null = &nulls[index];
        }

        if(!params.empty() && mysql_stmt_bind_param(statement.get(), binds.data()) != 0)
        {
            throw std::runtime_error{"entityfy/migrate:mysql: bind failed: " + error_text(handle_.get())};
        }

        if(mysql_stmt_execute(statement.get()) != 0)
        {
            throw std::runtime_error{"entityfy/migrate:mysql: execute failed: " + error_text(handle_.get())};
        }

        MYSQL_RES* raw_metadata = mysql_stmt_result_metadata(statement.get());
        std::vector<std::vector<dump_cell>> output;

        if(raw_metadata == nullptr)
        {
            return output;
        }

        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> metadata{raw_metadata, mysql_free_result};
        const auto column_count = mysql_num_fields(metadata.get());

        std::vector<MYSQL_BIND>    result_binds(column_count);
        std::vector<unsigned long> result_lengths(column_count, 0);
        std::vector<my_bool>       result_nulls(column_count, 0);
        std::vector<my_bool>       result_errors(column_count, 0);

        for(unsigned int index = 0; index < column_count; ++index)
        {
            result_binds[index] = MYSQL_BIND{};
            result_binds[index].buffer_type = MYSQL_TYPE_STRING;
            result_binds[index].buffer = nullptr;
            result_binds[index].buffer_length = 0;
            result_binds[index].length = &result_lengths[index];
            result_binds[index].is_null = &result_nulls[index];
            result_binds[index].error = &result_errors[index];
        }

        if(mysql_stmt_bind_result(statement.get(), result_binds.data()) != 0)
        {
            throw std::runtime_error{"entityfy/migrate:mysql: bind result failed: " + error_text(handle_.get())};
        }

        if(mysql_stmt_store_result(statement.get()) != 0)
        {
            throw std::runtime_error{"entityfy/migrate:mysql: store result failed: " + error_text(handle_.get())};
        }

        while(true)
        {
            const int status = mysql_stmt_fetch(statement.get());
            if(status == MYSQL_NO_DATA)
            {
                break;
            }

            if(status != 0 && status != MYSQL_DATA_TRUNCATED)
            {
                throw std::runtime_error{"entityfy/migrate:mysql: fetch failed: " + error_text(handle_.get())};
            }

            std::vector<dump_cell> current(column_count);

            for(unsigned int index = 0; index < column_count; ++index)
            {
                if(result_nulls[index] != 0)
                {
                    current[index].is_null = true;
                    continue;
                }

                std::string text(result_lengths[index], '\0');

                MYSQL_BIND column_bind{};
                column_bind.buffer_type = MYSQL_TYPE_STRING;
                column_bind.buffer = text.data();
                column_bind.buffer_length = static_cast<unsigned long>(text.size());

                unsigned long column_length = 0;
                my_bool       column_null = 0;
                column_bind.length = &column_length;
                column_bind.is_null = &column_null;

                if(mysql_stmt_fetch_column(statement.get(), &column_bind, index, 0) != 0)
                {
                    throw std::runtime_error{"entityfy/migrate:mysql: column fetch failed: " + error_text(handle_.get())};
                }

                text.resize(column_length);
                current[index].text = std::move(text);
            }

            output.emplace_back(std::move(current));
        }

        return output;
    }

private:
    mysql_handle handle_;
};

std::string column_definition(const column& value, bool alter, bool composite_key)
{
    std::string output = quote_ident(value.name) + " ";
    output += value.type.empty() ? "TEXT" : value.type;

    if(value.auto_increment)
    {
        output += " AUTO_INCREMENT";
    }

    if((value.not_null && !value.primary_key) || (composite_key && value.primary_key))
    {
        output += " NOT NULL";
    }

    if(!value.default_sql.empty())
    {
        output += " DEFAULT " + value.default_sql;
    }

    if(value.primary_key && !alter && !composite_key)
    {
        output += " PRIMARY KEY";
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

std::string create_table_sql(const table& value, bool if_not_exists)
{
    const std::vector<std::string> key_columns = primary_key_columns(value);
    const bool composite_key = key_columns.size() >= 2;

    std::string output = "CREATE TABLE ";
    if(if_not_exists)
    {
        output += "IF NOT EXISTS ";
    }

    output += quote_ident(value.name) + " (";

    for(std::size_t index = 0; index < value.columns.size(); ++index)
    {
        if(index != 0)
        {
            output += ", ";
        }

        output += column_definition(value.columns[index], false, composite_key);
    }

    if(composite_key)
    {
        output += ", PRIMARY KEY (";
        for(std::size_t index = 0; index < key_columns.size(); ++index)
        {
            if(index != 0)
            {
                output += ", ";
            }

            output += quote_ident(key_columns[index]);
        }

        output += ")";
    }

    output += ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    return output;
}

std::string create_index_sql(const table& value, const index& entry)
{
    std::string output = entry.unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ";
    output += quote_ident(entry.name) + " ON " + quote_ident(value.name) + " (";

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

std::vector<std::string> split_columns(std::string_view value)
{
    std::vector<std::string> output;
    std::size_t              start = 0;

    while(start <= value.size())
    {
        const auto comma = value.find(',', start);
        const auto end = comma == std::string_view::npos ? value.size() : comma;
        std::string column{value.substr(start, end - start)};

        if(!column.empty())
        {
            output.emplace_back(std::move(column));
        }

        if(comma == std::string_view::npos)
        {
            break;
        }

        start = comma + 1;
    }

    return output;
}

[[nodiscard]] bool is_numeric_type(std::string_view type)
{
    const auto base = type.substr(0, type.find('('));
    return base == "INTEGER" || base == "BIGINT" || base == "BOOLEAN" || base == "DECIMAL" ||
           base == "DOUBLE" || base == "FLOAT";
}

[[nodiscard]] bool is_binary_type(std::string_view type)
{
    const auto base = type.substr(0, type.find('('));
    return base == "BINARY" || base == "VARBINARY" || base == "BLOB" || base == "TINYBLOB" ||
           base == "MEDIUMBLOB" || base == "LONGBLOB";
}

[[nodiscard]] std::string hex_literal(std::string_view value)
{
    static constexpr char digits[] = "0123456789abcdef";

    std::string output = "0x";
    output.reserve(value.size() * 2 + 2);

    for(const unsigned char byte: value)
    {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 0x0F]);
    }

    return output;
}

[[nodiscard]] std::string dump_value(const dump_cell& cell, std::string_view type)
{
    if(cell.is_null)
    {
        return "NULL";
    }

    if(is_numeric_type(type))
    {
        return cell.text;
    }

    if(is_binary_type(type))
    {
        return hex_literal(cell.text);
    }

    return quote_literal(cell.text);
}

class mysql_provider final : public provider
{
public:
    mysql_provider(std::string uri, provider_options options)
        : options_{options}, connection_{uri}
    {
    }

    [[nodiscard]] std::string name() const override
    {
        return "mysql";
    }

    [[nodiscard]] diff_options comparison_options() const override
    {
        return diff_options{.ignore_checks = true};
    }

    [[nodiscard]] schema introspect() override
    {
        schema output;

        const auto tables = connection_.query(
            "SELECT table_name FROM information_schema.tables "
            "WHERE table_schema = DATABASE() AND table_type = 'BASE TABLE' "
            "AND table_name <> 'entityfy_schema_migrations' ORDER BY table_name");

        for(const auto& table_row: tables)
        {
            if(table_row.empty())
            {
                continue;
            }

            table current;
            current.name = table_row.front();

            const auto columns = connection_.query_params(
                "SELECT column_name, column_type, is_nullable, column_default, extra, column_key "
                "FROM information_schema.columns "
                "WHERE table_schema = DATABASE() AND table_name = ? "
                "ORDER BY ordinal_position",
                {current.name});

            for(const auto& column_row: columns)
            {
                if(column_row.size() < 6)
                {
                    continue;
                }

                column entry;
                entry.name = column_row[0];
                entry.type = normalize_mysql_type(column_row[1]);
                entry.not_null = column_row[2] == "NO";
                entry.default_sql = normalize_mysql_default(column_row[3]);
                entry.auto_increment = lower_copy(column_row[4]).find("auto_increment") != std::string::npos;
                entry.primary_key = column_row[5] == "PRI";

                if(entry.primary_key)
                {
                    entry.not_null = true;
                }

                current.columns.emplace_back(std::move(entry));
            }

            const auto primary_keys = connection_.query_params(
                "SELECT column_name FROM information_schema.statistics "
                "WHERE table_schema = DATABASE() AND table_name = ? AND index_name = 'PRIMARY' "
                "ORDER BY seq_in_index",
                {current.name});

            for(const auto& key_row: primary_keys)
            {
                if(!key_row.empty())
                {
                    current.primary_key.push_back(key_row.front());
                }
            }

            const auto indexes = connection_.query_params(
                "SELECT index_name, non_unique, GROUP_CONCAT(column_name ORDER BY seq_in_index) "
                "FROM information_schema.statistics "
                "WHERE table_schema = DATABASE() AND table_name = ? AND index_name <> 'PRIMARY' "
                "GROUP BY index_name, non_unique ORDER BY index_name",
                {current.name});

            for(const auto& index_row: indexes)
            {
                if(index_row.size() < 3)
                {
                    continue;
                }

                index entry;
                entry.name = index_row[0];
                entry.unique = index_row[1] == "0";
                entry.columns = split_columns(index_row[2]);

                if(!entry.columns.empty())
                {
                    current.indexes.emplace_back(std::move(entry));
                }
            }

            const auto foreign_keys = connection_.query_params(
                "SELECT k.column_name, k.referenced_table_name, k.referenced_column_name, "
                "r.update_rule, r.delete_rule, k.constraint_name "
                "FROM information_schema.key_column_usage k "
                "JOIN information_schema.referential_constraints r "
                "ON r.constraint_name = k.constraint_name AND r.constraint_schema = k.table_schema "
                "WHERE k.table_schema = DATABASE() AND k.table_name = ? AND k.referenced_table_name IS NOT NULL "
                "ORDER BY k.constraint_name, k.ordinal_position",
                {current.name});

            for(const auto& key_row: foreign_keys)
            {
                if(key_row.size() < 6)
                {
                    continue;
                }

                // RESTRICT is MySQL's implicit NO ACTION.
                const auto action = [](const std::string& value) {
                    return value == "NO ACTION" || value == "RESTRICT" ? std::string{} : value;
                };

                foreign_key entry;
                entry.column = key_row[0];
                entry.ref_table = key_row[1];
                entry.ref_column = key_row[2];
                entry.on_update = action(key_row[3]);
                entry.on_delete = action(key_row[4]);

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
        diff_options effective = options;
        effective.ignore_checks = true;

        plan output;
        const auto operations = diff_schemas(desired, actual, effective);
        output.destructive = has_destructive(operations);

        if(operations.empty())
        {
            return output;
        }

        std::vector<const table*> to_create;
        for(const auto& value: operations)
        {
            if(value.kind == op_kind::create_table)
            {
                if(const table* target = find_table(desired, value.table))
                {
                    to_create.push_back(target);
                }
            }
        }

        for(const table* target: order_for_creation(to_create))
        {
            step create;
            create.sql = create_table_sql(*target, true);
            output.steps.emplace_back(std::move(create));

            for(const auto& entry: target->indexes)
            {
                step index_step;
                index_step.sql = create_index_sql(*target, entry);
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

            for(const auto& value: found->second)
            {
                switch(value.kind)
                {
                case op_kind::rename_column:
                {
                    step rename;
                    rename.sql = "ALTER TABLE " + quote_ident(desired_table.name) + " RENAME COLUMN " +
                                 quote_ident(value.old_name) + " TO " + quote_ident(value.new_name);
                    output.steps.emplace_back(std::move(rename));
                    break;
                }
                case op_kind::add_column:
                {
                    step add;
                    add.sql = "ALTER TABLE " + quote_ident(desired_table.name) + " ADD COLUMN " +
                              column_definition(value.column_spec, true, false);
                    output.steps.emplace_back(std::move(add));
                    break;
                }
                case op_kind::drop_column:
                {
                    step drop;
                    drop.sql = "ALTER TABLE " + quote_ident(desired_table.name) + " DROP COLUMN " +
                               quote_ident(value.column);
                    output.steps.emplace_back(std::move(drop));
                    break;
                }
                case op_kind::alter_column:
                {
                    const column* before = find_column(*actual_table, value.column);
                    if(before == nullptr)
                    {
                        break;
                    }

                    if(value.column_spec.primary_key != before->primary_key ||
                       value.column_spec.auto_increment != before->auto_increment)
                    {
                        throw std::runtime_error{
                            "entityfy/migrate:mysql: primary key/auto_increment change requires a manual migration: " +
                            desired_table.name + "." + value.column
                        };
                    }

                    if(!same_type(value.column_spec.type, before->type) ||
                       value.column_spec.not_null != before->not_null ||
                       !same_default(value.column_spec.default_sql, before->default_sql))
                    {
                        step alter;
                        alter.sql = "ALTER TABLE " + quote_ident(desired_table.name) + " MODIFY COLUMN " +
                                    column_definition(value.column_spec, true, false);
                        output.steps.emplace_back(std::move(alter));
                    }

                    if(!effective.ignore_checks &&
                       normalize_sql(value.column_spec.check_sql) != normalize_sql(before->check_sql))
                    {
                        output.warnings.push_back("check constraint changes are not migrated automatically: " +
                                                  desired_table.name + "." + value.column);
                    }
                    break;
                }
                case op_kind::add_index:
                {
                    step add;
                    add.sql = create_index_sql(desired_table, value.index_spec);
                    output.steps.emplace_back(std::move(add));
                    break;
                }
                case op_kind::drop_index:
                {
                    step drop;
                    drop.sql = "DROP INDEX " + quote_ident(value.index_spec.name) + " ON " +
                               quote_ident(desired_table.name);
                    output.steps.emplace_back(std::move(drop));
                    break;
                }
                case op_kind::add_foreign_key:
                {
                    const std::string name = truncate_identifier(
                        "fk_" + desired_table.name + "_" + value.fk_spec.column);
                    std::string sql = "ALTER TABLE " + quote_ident(desired_table.name) + " ADD CONSTRAINT " +
                                      quote_ident(name) + " FOREIGN KEY (" + quote_ident(value.fk_spec.column) +
                                      ") REFERENCES " + quote_ident(value.fk_spec.ref_table) + " (" +
                                      quote_ident(value.fk_spec.ref_column) + ")";

                    if(!value.fk_spec.on_delete.empty())
                    {
                        sql += " ON DELETE " + value.fk_spec.on_delete;
                    }

                    if(!value.fk_spec.on_update.empty())
                    {
                        sql += " ON UPDATE " + value.fk_spec.on_update;
                    }

                    step add;
                    add.sql = std::move(sql);
                    output.steps.emplace_back(std::move(add));
                    break;
                }
                case op_kind::drop_foreign_key:
                {
                    const auto rows = connection_.query_params(
                        "SELECT constraint_name FROM information_schema.key_column_usage "
                        "WHERE table_schema = DATABASE() AND table_name = ? AND column_name = ? "
                        "AND referenced_table_name IS NOT NULL LIMIT 1",
                        {desired_table.name, value.fk_spec.column});

                    if(rows.empty() || rows.front().empty())
                    {
                        output.warnings.push_back("foreign key constraint not found for " + desired_table.name +
                                                  "." + value.fk_spec.column);
                        break;
                    }

                    step drop;
                    drop.sql = "ALTER TABLE " + quote_ident(desired_table.name) + " DROP FOREIGN KEY " +
                               quote_ident(rows.front().front());
                    output.steps.emplace_back(std::move(drop));
                    break;
                }
                case op_kind::create_table:
                case op_kind::drop_table:
                    break;
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
                output.steps.emplace_back(std::move(drop));
            }
        }

        return output;
    }

    void start() override
    {
        // MySQL DDL commits implicitly; the transaction protects the history
        // row, not the schema changes themselves.
        connection_.exec("START TRANSACTION");
    }

    void finish() override
    {
        connection_.exec("COMMIT");
    }

    void abort() override
    {
        try
        {
            connection_.exec("ROLLBACK");
        }
        catch(...)
        {
        }
    }

    void execute(const step& value) override
    {
        if(value.type == step::kind::sqlite_rebuild)
        {
            throw std::runtime_error{"entityfy/migrate:mysql: sqlite rebuild step is not valid for this provider"};
        }

        connection_.exec(value.sql);
    }

    void execute_sql(std::string_view sql) override
    {
        connection_.exec(sql);
    }

    void execute_params(std::string_view sql, const std::vector<std::string>& params) override
    {
        (void)connection_.query_params(sql, params);
    }

    [[nodiscard]] std::vector<std::vector<std::string>> query(std::string_view sql) override
    {
        return connection_.query(sql);
    }

    [[nodiscard]] std::vector<std::vector<std::string>> query_params(
        std::string_view sql,
        const std::vector<std::string>& params) override
    {
        return connection_.query_params(sql, params);
    }

    [[nodiscard]] std::string_view backup_extension() const override
    {
        return ".sql";
    }

    void backup(std::string_view path) override
    {
        const auto schema = introspect();

        std::vector<const table*> tables;
        tables.reserve(schema.tables.size());
        for(const auto& value: schema.tables)
        {
            tables.push_back(&value);
        }

        const auto ordered = order_for_creation(tables);

        std::ofstream out{std::string{path}, std::ios::binary | std::ios::trunc};
        if(!out)
        {
            throw std::runtime_error{"entityfy/migrate:mysql: cannot open backup file `" + std::string{path} + "`"};
        }

        out << "-- entityfy migrate backup\n";
        out << "SET NAMES utf8mb4;\n";
        out << "SET SESSION sql_mode = CONCAT_WS(',', @@SESSION.sql_mode, 'ANSI_QUOTES', 'NO_BACKSLASH_ESCAPES');\n";
        out << "SET FOREIGN_KEY_CHECKS=0;\n";
        out << "START TRANSACTION;\n\n";

        for(const table* value: ordered)
        {
            out << create_table_sql(*value, false) << ";\n";
        }

        for(const table* value: ordered)
        {
            for(const auto& entry: value->indexes)
            {
                out << create_index_sql(*value, entry) << ";\n";
            }
        }

        out << "\n";

        for(const table* value: ordered)
        {
            const auto rows = connection_.query_dump("SELECT * FROM " + quote_ident(value->name));

            for(const auto& row: rows)
            {
                out << "INSERT INTO " << quote_ident(value->name) << " (";

                for(std::size_t index = 0; index < value->columns.size(); ++index)
                {
                    if(index != 0)
                    {
                        out << ", ";
                    }

                    out << quote_ident(value->columns[index].name);
                }

                out << ") VALUES (";

                for(std::size_t index = 0; index < row.size() && index < value->columns.size(); ++index)
                {
                    if(index != 0)
                    {
                        out << ", ";
                    }

                    out << dump_value(row[index], value->columns[index].type);
                }

                out << ");\n";
            }
        }

        out << "\nCOMMIT;\nSET FOREIGN_KEY_CHECKS=1;\n";

        if(!out)
        {
            throw std::runtime_error{"entityfy/migrate:mysql: writing backup `" + std::string{path} + "` failed"};
        }
    }

    [[nodiscard]] std::vector<std::string> render_rebuild(const table&, const table&) override
    {
        return {};
    }

    void close() override
    {
    }

private:
    [[nodiscard]] std::vector<const table*> order_for_creation(const std::vector<const table*>& tables) const
    {
        std::vector<const table*> remaining = tables;
        std::vector<const table*> ordered;
        std::set<std::string>     created;

        while(!remaining.empty())
        {
            bool progress = false;

            for(auto iterator = remaining.begin(); iterator != remaining.end();)
            {
                const table* candidate = *iterator;
                bool         ready = true;

                for(const auto& key: candidate->foreign_keys)
                {
                    if(key.ref_table == candidate->name || created.contains(key.ref_table))
                    {
                        continue;
                    }

                    for(const table* other: remaining)
                    {
                        if(other->name == key.ref_table)
                        {
                            ready = false;
                            break;
                        }
                    }

                    if(!ready)
                    {
                        break;
                    }
                }

                if(ready)
                {
                    ordered.push_back(candidate);
                    created.insert(candidate->name);
                    iterator = remaining.erase(iterator);
                    progress = true;
                }
                else
                {
                    ++iterator;
                }
            }

            if(!progress)
            {
                for(const table* current: remaining)
                {
                    ordered.push_back(current);
                }

                break;
            }
        }

        return ordered;
    }

    provider_options options_;
    mysql_connection connection_;
};

} // namespace

std::unique_ptr<provider> open_mysql_provider(std::string uri, const provider_options& options)
{
    return std::make_unique<mysql_provider>(std::move(uri), options);
}

} // namespace entityfy::migrate
