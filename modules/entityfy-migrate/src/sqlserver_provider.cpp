#include <entityfy/migrate/provider.hpp>

// The URI grammar and the ODBC connection string live in core: the migrate
// provider and the runtime client must accept exactly the same URIs, so the
// parser is included rather than copied.
#include <entityfy/detail/sqlserver_uri.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>

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

std::string upper_copy(std::string_view input)
{
    std::string output{input};
    for(char& character: output)
    {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }

    return output;
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

std::string truncate_identifier(std::string value)
{
    if(value.size() > 128)
    {
        value.resize(128);
    }

    return value;
}

// SQLWCHAR is wchar_t on Windows but unsigned short on Unix; std::wstring is
// not portable there. A trailing NUL makes SQL_NTS work, and the size excludes
// it for binding.
[[nodiscard]] std::vector<SQLWCHAR> utf8_to_utf16(std::string_view input)
{
    std::vector<SQLWCHAR> output;
    output.reserve(input.size() + 1);

    for(std::size_t index = 0; index < input.size();)
    {
        const auto    first = static_cast<unsigned char>(input[index]);
        std::uint32_t code = 0;
        std::size_t   length = 1;

        if(first < 0x80)
        {
            code = first;
        }
        else if((first & 0xE0) == 0xC0)
        {
            code = first & 0x1F;
            length = 2;
        }
        else if((first & 0xF0) == 0xE0)
        {
            code = first & 0x0F;
            length = 3;
        }
        else if((first & 0xF8) == 0xF0)
        {
            code = first & 0x07;
            length = 4;
        }
        else
        {
            output.push_back(static_cast<SQLWCHAR>(0xFFFD));
            ++index;
            continue;
        }

        if(index + length > input.size())
        {
            output.push_back(static_cast<SQLWCHAR>(0xFFFD));
            break;
        }

        for(std::size_t offset = 1; offset < length; ++offset)
        {
            code = (code << 6U) | (static_cast<unsigned char>(input[index + offset]) & 0x3FU);
        }

        index += length;

        if(code <= 0xFFFF)
        {
            output.push_back(static_cast<SQLWCHAR>(code));
        }
        else
        {
            code -= 0x10000;
            output.push_back(static_cast<SQLWCHAR>(0xD800 + (code >> 10U)));
            output.push_back(static_cast<SQLWCHAR>(0xDC00 + (code & 0x3FFU)));
        }
    }

    output.push_back(0);
    return output;
}

[[nodiscard]] std::string utf16_to_utf8(const SQLWCHAR* input, std::size_t length)
{
    std::string output;
    output.reserve(length);

    const auto append = [&](std::uint32_t code) {
        if(code <= 0x7F)
        {
            output.push_back(static_cast<char>(code));
        }
        else if(code <= 0x7FF)
        {
            output.push_back(static_cast<char>(0xC0 | (code >> 6U)));
            output.push_back(static_cast<char>(0x80 | (code & 0x3FU)));
        }
        else if(code <= 0xFFFF)
        {
            output.push_back(static_cast<char>(0xE0 | (code >> 12U)));
            output.push_back(static_cast<char>(0x80 | ((code >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80 | (code & 0x3FU)));
        }
        else
        {
            output.push_back(static_cast<char>(0xF0 | (code >> 18U)));
            output.push_back(static_cast<char>(0x80 | ((code >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80 | ((code >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80 | (code & 0x3FU)));
        }
    };

    for(std::size_t index = 0; index < length; ++index)
    {
        std::uint32_t code = static_cast<std::uint32_t>(input[index]);

        if(code >= 0xD800 && code <= 0xDBFF && index + 1 < length)
        {
            const std::uint32_t low = static_cast<std::uint32_t>(input[index + 1]);
            if(low >= 0xDC00 && low <= 0xDFFF)
            {
                code = 0x10000 + ((code - 0xD800) << 10U) + (low - 0xDC00);
                ++index;
            }
        }

        append(code);
    }

    return output;
}

[[nodiscard]] bool succeeded(SQLRETURN result)
{
    return result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO;
}

[[nodiscard]] std::string diagnostics(SQLSMALLINT type, SQLHANDLE handle)
{
    SQLCHAR     state[7]{};
    SQLCHAR     text[1024]{};
    SQLINTEGER  native = 0;
    SQLSMALLINT length = 0;

    if(SQLGetDiagRec(type, handle, 1, state, &native, text, sizeof(text), &length) == SQL_SUCCESS)
    {
        std::string output{reinterpret_cast<const char*>(text)};
        output.append(" [");
        output.append(reinterpret_cast<const char*>(state));
        output.append(" native ");
        output.append(std::to_string(native));
        output.push_back(']');
        return output;
    }

    return "unknown ODBC error";
}

struct dump_cell
{
    bool        is_null = false;
    std::string text;
};

class sqlserver_connection
{
public:
    explicit sqlserver_connection(std::string_view uri)
    {
        const auto options = entityfy::detail::parse_sqlserver_uri(uri);
        database_ = options.database;

        if(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_) != SQL_SUCCESS)
        {
            throw std::runtime_error{"entityfy/migrate:sqlserver: ODBC environment allocation failed"};
        }

        SQLSetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);

        if(SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_) != SQL_SUCCESS)
        {
            throw std::runtime_error{"entityfy/migrate:sqlserver: ODBC connection allocation failed"};
        }

        auto wide = utf8_to_utf16(entityfy::detail::sqlserver_connection_string(options));
        SQLWCHAR   output[1024]{};
        SQLSMALLINT output_length = 0;

        if(!succeeded(SQLDriverConnectW(
               connection_,
               nullptr,
               wide.data(),
               SQL_NTS,
               output,
               static_cast<SQLSMALLINT>(sizeof(output) / sizeof(output[0])),
               &output_length,
               SQL_DRIVER_NOPROMPT
           )))
        {
            throw std::runtime_error{"entityfy/migrate:sqlserver: connection failed: " + diagnostics(SQL_HANDLE_DBC, connection_)};
        }

        exec("SET QUOTED_IDENTIFIER ON");
        exec("SET ANSI_NULLS ON");
        exec("SET ANSI_PADDING ON");
    }

    sqlserver_connection(const sqlserver_connection&) = delete;
    sqlserver_connection& operator=(const sqlserver_connection&) = delete;

    ~sqlserver_connection()
    {
        if(connection_ != SQL_NULL_HDBC)
        {
            SQLDisconnect(connection_);
            SQLFreeHandle(SQL_HANDLE_DBC, connection_);
        }

        if(environment_ != SQL_NULL_HENV)
        {
            SQLFreeHandle(SQL_HANDLE_ENV, environment_);
        }
    }

    void exec(std::string_view sql)
    {
        SQLHSTMT statement = SQL_NULL_HSTMT;
        if(SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement) != SQL_SUCCESS)
        {
            throw std::runtime_error{"entityfy/migrate:sqlserver: statement allocation failed"};
        }

        auto wide = utf8_to_utf16(sql);

        if(!succeeded(SQLExecDirectW(statement, wide.data(), SQL_NTS)))
        {
            const auto message = diagnostics(SQL_HANDLE_STMT, statement);
            SQLFreeHandle(SQL_HANDLE_STMT, statement);
            throw std::runtime_error{"entityfy/migrate:sqlserver: `" + std::string{sql} + "` failed: " + message};
        }

        // SQL Server can return the first result of a long-running command
        // (BACKUP DATABASE is the one we rely on) before the command has
        // completely finished. Draining every result keeps exec synchronous:
        // a backup is readable as soon as this method returns.
        for(SQLRETURN more = SQLMoreResults(statement); more != SQL_NO_DATA;
            more = SQLMoreResults(statement))
        {
            if(!succeeded(more))
            {
                const auto message = diagnostics(SQL_HANDLE_STMT, statement);
                SQLFreeHandle(SQL_HANDLE_STMT, statement);
                throw std::runtime_error{"entityfy/migrate:sqlserver: `" + std::string{sql} + "` failed: " + message};
            }
        }

        SQLFreeHandle(SQL_HANDLE_STMT, statement);
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
        SQLHSTMT statement = SQL_NULL_HSTMT;
        if(SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement) != SQL_SUCCESS)
        {
            throw std::runtime_error{"entityfy/migrate:sqlserver: statement allocation failed"};
        }

        auto wide = utf8_to_utf16(sql);

        // Parameterless statements go through the text protocol: statements
        // such as RESTORE HEADERONLY are not preparable through SQLPrepare.
        SQLRETURN result = params.empty() ? SQLExecDirectW(statement, wide.data(), SQL_NTS)
                                          : SQLPrepareW(statement, wide.data(), SQL_NTS);

        if(!succeeded(result))
        {
            const auto message = diagnostics(SQL_HANDLE_STMT, statement);
            SQLFreeHandle(SQL_HANDLE_STMT, statement);
            throw std::runtime_error{"entityfy/migrate:sqlserver: prepare failed: " + message};
        }

        std::vector<std::vector<SQLWCHAR>> texts;
        texts.reserve(params.size());
        std::vector<SQLLEN> indicators(params.size(), 0);

        for(std::size_t index = 0; index < params.size(); ++index)
        {
            texts.push_back(utf8_to_utf16(params[index]));
            indicators[index] = static_cast<SQLLEN>((texts.back().size() - 1) * sizeof(SQLWCHAR));
        }

        for(std::size_t index = 0; index < params.size(); ++index)
        {
            if(!succeeded(SQLBindParameter(
                   statement,
                   static_cast<SQLUSMALLINT>(index + 1),
                   SQL_PARAM_INPUT,
                   SQL_C_WCHAR,
                   SQL_WVARCHAR,
                   static_cast<SQLULEN>(texts[index].size() - 1),
                   0,
                   texts[index].data(),
                   indicators[index],
                   &indicators[index]
               )))
            {
                const auto message = diagnostics(SQL_HANDLE_STMT, statement);
                SQLFreeHandle(SQL_HANDLE_STMT, statement);
                throw std::runtime_error{"entityfy/migrate:sqlserver: bind failed: " + message};
            }
        }

        if(!params.empty() && !succeeded(SQLExecute(statement)))
        {
            const auto message = diagnostics(SQL_HANDLE_STMT, statement);
            SQLFreeHandle(SQL_HANDLE_STMT, statement);
            throw std::runtime_error{"entityfy/migrate:sqlserver: execute failed: " + message};
        }

        SQLSMALLINT columns = 0;
        SQLNumResultCols(statement, &columns);
        std::vector<std::vector<dump_cell>> output;

        if(columns == 0)
        {
            SQLFreeHandle(SQL_HANDLE_STMT, statement);
            return output;
        }

        while(true)
        {
            const auto fetched = SQLFetch(statement);
            if(fetched == SQL_NO_DATA)
            {
                break;
            }

            if(!succeeded(fetched))
            {
                const auto message = diagnostics(SQL_HANDLE_STMT, statement);
                SQLFreeHandle(SQL_HANDLE_STMT, statement);
                throw std::runtime_error{"entityfy/migrate:sqlserver: fetch failed: " + message};
            }

            std::vector<dump_cell> current(columns);

            for(SQLSMALLINT index = 0; index < columns; ++index)
            {
                std::vector<SQLWCHAR> value;
                SQLWCHAR              buffer[2048];
                SQLLEN       indicator = 0;
                bool         is_null = false;

                while(true)
                {
                    const auto chunk = SQLGetData(
                        statement,
                        static_cast<SQLUSMALLINT>(index + 1),
                        SQL_C_WCHAR,
                        buffer,
                        sizeof(buffer),
                        &indicator
                    );

                    if(chunk == SQL_NULL_DATA)
                    {
                        is_null = true;
                        break;
                    }

                    if(chunk == SQL_NO_DATA)
                    {
                        break;
                    }

                    if(!succeeded(chunk))
                    {
                        break;
                    }

                    // ODBC reports NULL through the indicator, not the return code.
                    if(indicator == SQL_NULL_DATA)
                    {
                        is_null = true;
                        break;
                    }

                    const auto characters = indicator > 0 ? static_cast<std::size_t>(indicator / static_cast<SQLLEN>(sizeof(SQLWCHAR)))
                                                          : std::size_t{0};
                    value.insert(value.end(), buffer, buffer + characters);

                    if(chunk != SQL_SUCCESS_WITH_INFO)
                    {
                        break;
                    }
                }

                current[static_cast<std::size_t>(index)].is_null = is_null;
                current[static_cast<std::size_t>(index)].text = utf16_to_utf8(value.data(), value.size());
            }

            output.emplace_back(std::move(current));
        }

        SQLFreeHandle(SQL_HANDLE_STMT, statement);
        return output;
    }

    [[nodiscard]] const std::string& database() const noexcept
    {
        return database_;
    }

private:
    SQLHENV     environment_ = SQL_NULL_HENV;
    SQLHDBC     connection_ = SQL_NULL_HDBC;
    std::string database_;
};

std::string normalize_sqlserver_type(std::string type)
{
    const auto lowered = lower_copy(trim_copy(type));
    const auto open = lowered.find('(');
    const auto base = open == std::string::npos ? lowered : lowered.substr(0, open);
    const auto parameters = open == std::string::npos ? std::string{} : lowered.substr(open);

    if(base == "int" || base == "smallint" || base == "tinyint")
    {
        return "INTEGER";
    }

    if(base == "bigint")
    {
        return "BIGINT";
    }

    if(base == "bit")
    {
        return "BIT";
    }

    if(base == "float" || base == "real")
    {
        return "FLOAT";
    }

    if(base == "nvarchar" || base == "nchar" || base == "varchar" || base == "char" || base == "varbinary" ||
       base == "binary" || base == "decimal" || base == "numeric")
    {
        return upper_copy(base) + parameters;
    }

    if(base == "datetime2" || base == "datetime" || base == "date" || base == "time" || base == "datetimeoffset")
    {
        return upper_copy(base);
    }

    return upper_copy(base.empty() ? lowered : base);
}

// SQL Server wraps defaults in parentheses and exposes getdate() as the
// timestamp default.
std::string normalize_sqlserver_default(std::string value)
{
    value = trim_copy(value);

    while(value.size() >= 2 && value.front() == '(' && value.back() == ')')
    {
        value = trim_copy(value.substr(1, value.size() - 2));
    }

    if(value.empty())
    {
        return {};
    }

    const auto lowered = lower_copy(value);

    if(lowered == "getdate()" || lowered == "getutcdate()" || lowered == "sysdatetime()" || lowered == "current_timestamp")
    {
        return "CURRENT_TIMESTAMP";
    }

    if(lowered == "null")
    {
        return {};
    }

    return value;
}

std::string column_definition(const column& value, bool alter, bool composite_key)
{
    std::string output = quote_ident(value.name) + " ";
    output += value.type.empty() ? "NVARCHAR(MAX)" : value.type;

    if(value.auto_increment && !alter)
    {
        output += " IDENTITY(1,1)";
    }

    if(value.not_null || value.primary_key)
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

        const auto action = [](const std::string& value) -> std::string {
            return value == "RESTRICT" ? std::string{"NO ACTION"} : value;
        };

        if(!value.references->on_delete.empty())
        {
            output += " ON DELETE " + action(value.references->on_delete);
        }

        if(!value.references->on_update.empty())
        {
            output += " ON UPDATE " + action(value.references->on_update);
        }
    }

    return output;
}

std::string create_table_sql(const table& value, bool if_not_exists)
{
    const std::vector<std::string> key_columns = primary_key_columns(value);
    const bool composite_key = key_columns.size() >= 2;

    std::string output;
    if(if_not_exists)
    {
        output = "IF OBJECT_ID(N'";
        output += quote_ident(value.name);
        output += "', N'U') IS NULL ";
    }

    output += "CREATE TABLE " + quote_ident(value.name) + " (";

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

    output += ")";
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
    return base == "INTEGER" || base == "BIGINT" || base == "BIT" || base == "FLOAT" ||
           base == "DECIMAL" || base == "NUMERIC" || base == "MONEY" || base == "SMALLMONEY";
}

[[nodiscard]] bool is_binary_type(std::string_view type)
{
    const auto base = type.substr(0, type.find('('));
    return base == "BINARY" || base == "VARBINARY";
}

[[nodiscard]] std::string sqlserver_string_literal(std::string_view value)
{
    std::string output{"N'"};
    for(const char current: value)
    {
        if(current == '\'')
        {
            output += "''";
        }
        else
        {
            output.push_back(current);
        }
    }

    output.push_back('\'');
    return output;
}

std::string dump_value(const dump_cell& cell, std::string_view type)
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
        return cell.text.empty() ? std::string{"0x"} : "0x" + cell.text;
    }

    return "N" + quote_literal(cell.text);
}

// Binary columns are selected as hex text because SQLGetData would truncate
// them in the wide-character conversion.
[[nodiscard]] std::string dump_select(const table& value)
{
    std::string sql = "SELECT ";

    for(std::size_t index = 0; index < value.columns.size(); ++index)
    {
        if(index != 0)
        {
            sql += ", ";
        }

        const auto& entry = value.columns[index];

        if(is_binary_type(entry.type))
        {
            sql += "CONVERT(NVARCHAR(MAX), " + quote_ident(entry.name) + ", 2) AS " +
                   quote_ident(entry.name);
        }
        else
        {
            sql += quote_ident(entry.name);
        }
    }

    sql += " FROM " + quote_ident(value.name);
    return sql;
}

class sqlserver_provider final : public provider
{
public:
    sqlserver_provider(std::string uri, provider_options options)
        : options_{options}, connection_{uri}
    {
    }

    [[nodiscard]] std::string name() const override
    {
        return "sqlserver";
    }

    [[nodiscard]] diff_options comparison_options() const override
    {
        return diff_options{.ignore_checks = true};
    }

    [[nodiscard]] schema introspect() override
    {
        schema output;

        const auto tables = connection_.query(
            "SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES "
            "WHERE TABLE_TYPE = 'BASE TABLE' AND TABLE_NAME <> 'entityfy_schema_migrations' "
            "AND TABLE_SCHEMA = SCHEMA_NAME() ORDER BY TABLE_NAME");

        for(const auto& table_row: tables)
        {
            if(table_row.empty())
            {
                continue;
            }

            table current;
            current.name = table_row.front();

            const auto columns = connection_.query_params(
                "SELECT COLUMN_NAME, "
                "CASE "
                "  WHEN DATA_TYPE IN ('nvarchar','nchar','varchar','char','varbinary','binary') "
                "    THEN DATA_TYPE + '(' + CASE WHEN CHARACTER_MAXIMUM_LENGTH = -1 THEN 'max' "
                "         ELSE CAST(CHARACTER_MAXIMUM_LENGTH AS varchar(20)) END + ')' "
                "  WHEN DATA_TYPE IN ('decimal','numeric') "
                "    THEN DATA_TYPE + '(' + CAST(NUMERIC_PRECISION AS varchar(20)) + ',' + "
                "         CAST(NUMERIC_SCALE AS varchar(20)) + ')' "
                "  ELSE DATA_TYPE END, "
                "IS_NULLABLE, COALESCE(COLUMN_DEFAULT, ''), "
                "COLUMNPROPERTY(OBJECT_ID(TABLE_SCHEMA + '.' + TABLE_NAME), COLUMN_NAME, 'IsIdentity') "
                "FROM INFORMATION_SCHEMA.COLUMNS "
                "WHERE TABLE_NAME = ? AND TABLE_SCHEMA = SCHEMA_NAME() ORDER BY ORDINAL_POSITION",
                {current.name});

            for(const auto& column_row: columns)
            {
                if(column_row.size() < 5)
                {
                    continue;
                }

                column entry;
                entry.name = column_row[0];
                entry.type = normalize_sqlserver_type(column_row[1]);
                entry.not_null = column_row[2] == "NO";
                entry.default_sql = normalize_sqlserver_default(column_row[3]);
                entry.auto_increment = column_row[4] == "1";

                current.columns.emplace_back(std::move(entry));
            }

            const auto primary_keys = connection_.query_params(
                "SELECT k.COLUMN_NAME FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS t "
                "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE k ON k.CONSTRAINT_NAME = t.CONSTRAINT_NAME "
                "WHERE t.CONSTRAINT_TYPE = 'PRIMARY KEY' AND k.TABLE_NAME = ? AND k.TABLE_SCHEMA = SCHEMA_NAME() "
                "ORDER BY k.ORDINAL_POSITION",
                {current.name});

            for(const auto& key_row: primary_keys)
            {
                if(key_row.empty())
                {
                    continue;
                }

                current.primary_key.push_back(key_row.front());

                if(column* target = find_column(current, key_row.front()))
                {
                    target->primary_key = true;
                    target->not_null = true;
                }
            }

            const auto indexes = connection_.query_params(
                "SELECT i.name, i.is_unique, c.name FROM sys.indexes i "
                "JOIN sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id "
                "JOIN sys.columns c ON c.object_id = ic.object_id AND c.column_id = ic.column_id "
                "WHERE i.object_id = OBJECT_ID(?) AND i.is_primary_key = 0 "
                "ORDER BY i.name, ic.key_ordinal",
                {current.name});

            for(const auto& index_row: indexes)
            {
                if(index_row.size() < 3)
                {
                    continue;
                }

                const auto name = index_row[0];
                const bool unique = index_row[1] == "1";
                const auto column = index_row[2];

                const auto existing = std::find_if(current.indexes.begin(), current.indexes.end(), [&](const index& entry) {
                    return entry.name == name;
                });

                if(existing == current.indexes.end())
                {
                    current.indexes.emplace_back(index{.name = name, .columns = {column}, .unique = unique});
                }
                else
                {
                    existing->columns.emplace_back(column);
                }
            }

            // SQL Server's INFORMATION_SCHEMA.KEY_COLUMN_USAGE has no
            // POSITION_IN_UNIQUE_CONSTRAINT column (that is MySQL); the sys
            // catalog pairs each local column with its referenced column.
            const auto foreign_keys = connection_.query_params(
                "SELECT fk.name, pc.name, rt.name, rc.name, "
                "REPLACE(fk.update_referential_action_desc, '_', ' '), "
                "REPLACE(fk.delete_referential_action_desc, '_', ' ') "
                "FROM sys.foreign_keys fk "
                "JOIN sys.foreign_key_columns fkc ON fkc.constraint_object_id = fk.object_id "
                "JOIN sys.tables pt ON pt.object_id = fk.parent_object_id "
                "JOIN sys.columns pc ON pc.object_id = fkc.parent_object_id "
                "AND pc.column_id = fkc.parent_column_id "
                "JOIN sys.tables rt ON rt.object_id = fk.referenced_object_id "
                "JOIN sys.columns rc ON rc.object_id = fkc.referenced_object_id "
                "AND rc.column_id = fkc.referenced_column_id "
                "WHERE pt.name = ? AND SCHEMA_NAME(pt.schema_id) = SCHEMA_NAME() "
                "ORDER BY fk.name, fkc.constraint_column_id",
                {current.name});

            std::vector<std::pair<std::string, std::size_t>> key_indices;

            for(const auto& key_row: foreign_keys)
            {
                if(key_row.size() < 6)
                {
                    continue;
                }

                const auto action = [](const std::string& value) {
                    return value == "NO ACTION" || value == "RESTRICT" ? std::string{} : value;
                };

                const std::string& name = key_row[0];
                auto known = std::ranges::find_if(
                    key_indices, [&](const std::pair<std::string, std::size_t>& entry) { return entry.first == name; });

                if(known == key_indices.end())
                {
                    key_indices.emplace_back(name, current.foreign_keys.size());

                    foreign_key entry;
                    entry.ref_table = key_row[2];
                    entry.on_update = action(key_row[4]);
                    entry.on_delete = action(key_row[5]);
                    current.foreign_keys.emplace_back(std::move(entry));
                    known = key_indices.end() - 1;
                }

                foreign_key& entry = current.foreign_keys[known->second];
                entry.columns.push_back(key_row[1]);
                entry.ref_columns.push_back(key_row[3]);
            }

            for(auto& entry: current.foreign_keys)
            {
                if(entry.columns.empty())
                {
                    continue;
                }

                entry.column = entry.columns.front();
                entry.ref_column = entry.ref_columns.front();

                if(entry.columns.size() == 1)
                {
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
        require_single_column_foreign_keys(desired, "sqlserver");
        require_single_column_foreign_keys(actual, "sqlserver");

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
                    rename.sql = "EXEC sp_rename '" + desired_table.name + "." + value.old_name + "', '" +
                                 value.new_name + "', 'COLUMN'";
                    output.steps.emplace_back(std::move(rename));
                    break;
                }
                case op_kind::add_column:
                {
                    step add;
                    add.sql = "ALTER TABLE " + quote_ident(desired_table.name) + " ADD " +
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
                            "entityfy/migrate:sqlserver: primary key/identity change requires a manual migration: " +
                            desired_table.name + "." + value.column
                        };
                    }

                    if(!same_type(value.column_spec.type, before->type) ||
                       value.column_spec.not_null != before->not_null)
                    {
                        // SQL Server refuses ALTER COLUMN while an index uses the
                        // column, so the dependent indexes are rebuilt around it.
                        std::vector<index> dependent_indexes;
                        for(const auto& entry: actual_table->indexes)
                        {
                            if(std::ranges::find(entry.columns, value.column) != entry.columns.end())
                            {
                                dependent_indexes.push_back(entry);
                            }
                        }

                        for(const auto& entry: dependent_indexes)
                        {
                            step drop;
                            drop.sql = "DROP INDEX " + quote_ident(entry.name) + " ON " +
                                       quote_ident(desired_table.name);
                            output.steps.emplace_back(std::move(drop));
                        }

                        step alter;
                        alter.sql = "ALTER TABLE " + quote_ident(desired_table.name) + " ALTER COLUMN " +
                                    quote_ident(value.column) + " " +
                                    (value.column_spec.type.empty() ? "NVARCHAR(MAX)" : value.column_spec.type) +
                                    (value.column_spec.not_null ? " NOT NULL" : " NULL");
                        output.steps.emplace_back(std::move(alter));

                        for(const auto& entry: dependent_indexes)
                        {
                            step recreate;
                            recreate.sql = create_index_sql(desired_table, entry);
                            output.steps.emplace_back(std::move(recreate));
                        }
                    }

                    if(!same_default(value.column_spec.default_sql, before->default_sql))
                    {
                        const auto constraint_rows = connection_.query_params(
                            "SELECT dc.name FROM sys.default_constraints dc "
                            "JOIN sys.columns c ON c.object_id = dc.parent_object_id AND c.column_id = dc.parent_column_id "
                            "WHERE dc.parent_object_id = OBJECT_ID(?) AND c.name = ?",
                            {desired_table.name, value.column});

                        for(const auto& row: constraint_rows)
                        {
                            if(row.empty())
                            {
                                continue;
                            }

                            step drop;
                            drop.sql = "ALTER TABLE " + quote_ident(desired_table.name) + " DROP CONSTRAINT " +
                                       quote_ident(row.front());
                            output.steps.emplace_back(std::move(drop));
                        }

                        if(!value.column_spec.default_sql.empty())
                        {
                            step add;
                            add.sql = "ALTER TABLE " + quote_ident(desired_table.name) + " ADD CONSTRAINT " +
                                      quote_ident(truncate_identifier("df_" + desired_table.name + "_" + value.column)) +
                                      " DEFAULT " + value.column_spec.default_sql + " FOR " + quote_ident(value.column);
                            output.steps.emplace_back(std::move(add));
                        }
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
                    const std::string name = truncate_identifier("fk_" + desired_table.name + "_" + value.fk_spec.column);
                    std::string sql = "ALTER TABLE " + quote_ident(desired_table.name) + " ADD CONSTRAINT " +
                                      quote_ident(name) + " FOREIGN KEY (" + quote_ident(value.fk_spec.column) +
                                      ") REFERENCES " + quote_ident(value.fk_spec.ref_table) + " (" +
                                      quote_ident(value.fk_spec.ref_column) + ")";

                    if(!value.fk_spec.on_delete.empty())
                    {
                        sql += " ON DELETE " +
                               (value.fk_spec.on_delete == "RESTRICT" ? std::string{"NO ACTION"} : value.fk_spec.on_delete);
                    }

                    if(!value.fk_spec.on_update.empty())
                    {
                        sql += " ON UPDATE " +
                               (value.fk_spec.on_update == "RESTRICT" ? std::string{"NO ACTION"} : value.fk_spec.on_update);
                    }

                    step add;
                    add.sql = std::move(sql);
                    output.steps.emplace_back(std::move(add));
                    break;
                }
                case op_kind::drop_foreign_key:
                {
                    const auto rows = connection_.query_params(
                        "SELECT TOP (1) CONSTRAINT_NAME FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE "
                        "WHERE TABLE_NAME = ? AND TABLE_SCHEMA = SCHEMA_NAME() AND COLUMN_NAME = ?",
                        {desired_table.name, value.fk_spec.column});

                    if(rows.empty() || rows.front().empty())
                    {
                        output.warnings.push_back("foreign key constraint not found for " + desired_table.name +
                                                  "." + value.fk_spec.column);
                        break;
                    }

                    step drop;
                    drop.sql = "ALTER TABLE " + quote_ident(desired_table.name) + " DROP CONSTRAINT " +
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
        // The isolation level is deliberately left at the server default: the
        // migrate provider exposes no isolation option, and the runtime client
        // (entityfy::client) is the component that owns isolation selection.
        // Setting a level here would make a migration run under a different
        // contract than the application that follows it.
        connection_.exec("BEGIN TRANSACTION");
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
            throw std::runtime_error{"entityfy/migrate:sqlserver: sqlite rebuild step is not valid for this provider"};
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

    [[nodiscard]] bool supports_native_backup() const noexcept override
    {
        return true;
    }

    void backup_native(std::string_view server_path) override
    {
        if(server_path.empty())
        {
            throw std::invalid_argument{"entityfy/migrate:sqlserver: native backup path is empty"};
        }

        // BACKUP DATABASE writes on the server, so the caller supplies a path
        // that exists there and is writable by the SQL Server service account.
        const std::string sql = "BACKUP DATABASE " + quote_ident(connection_.database()) + " TO DISK = " +
                                sqlserver_string_literal(server_path) + " WITH COPY_ONLY, FORMAT, CHECKSUM";
        connection_.exec(sql);
    }

    void backup(std::string_view path) override
    {
        // This stays the portable logical dump the engine uses: it can be read,
        // diffed and replayed through any provider, which is what rollback and
        // restore_dump rely on. The opt-in backup_native() above writes a
        // server-side .bak when an operator explicitly asks for it.
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
            throw std::runtime_error{"entityfy/migrate:sqlserver: cannot open backup file `" + std::string{path} + "`"};
        }

        out << "-- entityfy migrate backup\n";
        out << "SET NOCOUNT ON;\n";
        out << "SET XACT_ABORT ON;\n";
        out << "BEGIN TRANSACTION;\n\n";

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
            const auto rows = connection_.query_dump(dump_select(*value));

            if(rows.empty())
            {
                continue;
            }

            const bool identity = std::ranges::any_of(
                value->columns, [](const column& entry) { return entry.auto_increment; });

            if(identity)
            {
                out << "SET IDENTITY_INSERT " << quote_ident(value->name) << " ON;\n";
            }

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

            if(identity)
            {
                out << "SET IDENTITY_INSERT " << quote_ident(value->name) << " OFF;\n";
            }
        }

        out << "\nCOMMIT;\n";

        if(!out)
        {
            throw std::runtime_error{"entityfy/migrate:sqlserver: writing backup `" + std::string{path} + "` failed"};
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

    provider_options    options_;
    sqlserver_connection connection_;
};

} // namespace

std::unique_ptr<provider> open_sqlserver_provider(std::string uri, const provider_options& options)
{
    return std::make_unique<sqlserver_provider>(std::move(uri), options);
}

} // namespace entityfy::migrate
