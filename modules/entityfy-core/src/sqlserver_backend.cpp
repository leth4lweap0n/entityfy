#include <entityfy/detail/backend.hpp>
#include <entityfy/detail/sqlserver_uri.hpp>
#include <entityfy/error.hpp>
#include <entityfy/statement.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace entityfy::detail
{
namespace
{

// SQLWCHAR is wchar_t on Windows but unsigned short on Unix; a std::wstring is
// therefore not portable (wchar_t is 32-bit there). The returned vector carries
// a trailing NUL so SQL_NTS works, and its size excludes that sentinel for
// binding.
[[nodiscard]] std::vector<SQLWCHAR> utf8_to_utf16(std::string_view input)
{
    std::vector<SQLWCHAR> output;
    output.reserve(input.size() + 1);

    for(std::size_t index = 0; index < input.size();)
    {
        const auto first = static_cast<unsigned char>(input[index]);
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

struct environment_handle
{
    SQLHENV value = SQL_NULL_HENV;

    environment_handle()
    {
        if(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &value) != SQL_SUCCESS)
        {
            throw std::runtime_error{"entityfy SQL Server ODBC environment allocation failed"};
        }

        SQLSetEnvAttr(value, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
    }

    environment_handle(const environment_handle&) = delete;
    environment_handle& operator=(const environment_handle&) = delete;

    ~environment_handle()
    {
        if(value != SQL_NULL_HENV)
        {
            SQLFreeHandle(SQL_HANDLE_ENV, value);
        }
    }
};

[[nodiscard]] environment_handle& shared_environment()
{
    static environment_handle environment;
    return environment;
}

struct diagnostics
{
    std::string         state;
    std::string         message;
    SQLINTEGER          native = 0;
    sqlserver_error_kind kind = sqlserver_error_kind::none;
};

[[nodiscard]] diagnostics read_diagnostics(SQLSMALLINT type, SQLHANDLE handle)
{
    diagnostics output;
    SQLCHAR     state[7]{};
    SQLCHAR     text[1024]{};
    SQLSMALLINT length = 0;

    if(SQLGetDiagRec(type, handle, 1, state, &output.native, text, sizeof(text), &length) == SQL_SUCCESS)
    {
        output.state = reinterpret_cast<const char*>(state);
        output.message = reinterpret_cast<const char*>(text);
    }

    output.kind = classify_sqlserver_native(output.native);
    return output;
}

// The human-readable cause, prefixed by what the native number means so the
// message alone is already diagnostic. The `[state native N]` suffix stays so a
// caller can correlate the failure with the server's own log.
[[nodiscard]] diagnostics describe_sqlserver_error(SQLSMALLINT type, SQLHANDLE handle)
{
    auto detail = read_diagnostics(type, handle);

    switch(detail.kind)
    {
        case sqlserver_error_kind::duplicate_key:
            detail.message.insert(0, "duplicate key: ");
            break;
        case sqlserver_error_kind::foreign_key:
            // 547 covers both FOREIGN KEY and CHECK conflicts; the message is
            // the only discriminator the server gives.
            detail.message.insert(
                0,
                detail.message.find("FOREIGN KEY") != std::string::npos ? "foreign key constraint: "
                                                                        : "check constraint: "
            );
            break;
        case sqlserver_error_kind::not_null:
            detail.message.insert(0, "not null: ");
            break;
        case sqlserver_error_kind::deadlock:
            detail.message.insert(0, "deadlock victim: ");
            break;
        case sqlserver_error_kind::transient:
            detail.message.insert(0, "transient: ");
            break;
        case sqlserver_error_kind::authentication:
            detail.message.insert(0, "authentication failed: ");
            break;
        case sqlserver_error_kind::connection:
            detail.message.insert(0, "connection: ");
            break;
        case sqlserver_error_kind::check:
            detail.message.insert(0, "check constraint: ");
            break;
        case sqlserver_error_kind::none:
            break;
    }

    detail.message.append(" [");
    detail.message.append(detail.state);
    detail.message.append(" native ");
    detail.message.append(std::to_string(detail.native));
    detail.message.push_back(']');
    return detail;
}

// One place that turns a classified diagnostics record into the entityfy error
// type: retryable conditions become transient types, credential and connection
// failures become connection_error, and a constraint conflict keeps its type.
// Every sql_error subclass carries the SQL text that failed.
[[noreturn]] void throw_sqlserver_error(std::string_view operation, const std::string& sql, const diagnostics& detail)
{
    auto message = sql_context_message("SQL Server", operation, sql, detail.message);

    switch(detail.kind)
    {
        case sqlserver_error_kind::duplicate_key:
            throw unique_violation{std::move(message), sql};
        case sqlserver_error_kind::foreign_key:
            if(detail.message.find("FOREIGN KEY") != std::string::npos)
            {
                throw foreign_key_violation{std::move(message), sql};
            }

            throw check_violation{std::move(message), sql};
        case sqlserver_error_kind::not_null:
            throw not_null_violation{std::move(message), sql};
        case sqlserver_error_kind::check:
            throw check_violation{std::move(message), sql};
        case sqlserver_error_kind::deadlock:
            throw deadlock_detected{std::move(message), sql};
        case sqlserver_error_kind::transient:
            throw transient_error{std::move(message), sql};
        case sqlserver_error_kind::authentication:
            throw connection_error{std::move(message)};
        case sqlserver_error_kind::connection:
            throw connection_error{std::move(message)};
        case sqlserver_error_kind::none:
            break;
    }

    // Unmapped numbers keep the message-based classification so a server that
    // words a constraint failure differently still lands on a typed error.
    throw_classified_sql_error("SQL Server", operation, sql, detail.message);
}

[[nodiscard]] bool succeeded(SQLRETURN result)
{
    return result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO;
}

struct sqlserver_parameters
{
    std::deque<std::vector<SQLWCHAR>> texts;
    std::deque<SQLBIGINT>         integers;
    std::deque<SQLUBIGINT>        unsigned_integers;
    std::deque<double>            reals;
    std::deque<unsigned char>     bits;
    std::deque<std::vector<char>> binaries;
    std::vector<SQLPOINTER>       buffers;
    std::vector<SQLLEN>           lengths;
    std::vector<SQLLEN>           indicators;
    std::vector<SQLSMALLINT>      c_types;
    std::vector<SQLSMALLINT>      sql_types;
    std::vector<SQLULEN>          column_sizes;
    std::vector<SQLSMALLINT>      decimal_digits;
};

[[nodiscard]] sqlserver_parameters make_sqlserver_parameters(const std::vector<sql_value>& values)
{
    const auto count = values.size();
    sqlserver_parameters output;
    output.buffers.resize(count, nullptr);
    output.lengths.resize(count, 0);
    output.indicators.resize(count, 0);
    output.c_types.resize(count, SQL_C_DEFAULT);
    output.sql_types.resize(count, SQL_VARCHAR);
    output.column_sizes.resize(count, 0);
    output.decimal_digits.resize(count, 0);

    for(std::size_t index = 0; index < count; ++index)
    {
        std::visit(
            [&](const auto& current) {
                using current_type = std::remove_cvref_t<decltype(current)>;

                if constexpr(std::same_as<current_type, std::monostate>)
                {
                    output.c_types[index] = SQL_C_CHAR;
                    output.sql_types[index] = SQL_VARCHAR;
                    output.indicators[index] = SQL_NULL_DATA;
                }
                else if constexpr(std::same_as<current_type, bool>)
                {
                    output.bits.push_back(current ? 1 : 0);
                    output.c_types[index] = SQL_C_BIT;
                    output.sql_types[index] = SQL_BIT;
                    output.buffers[index] = &output.bits.back();
                }
                else if constexpr(std::same_as<current_type, std::int64_t>)
                {
                    output.integers.push_back(current);
                    output.c_types[index] = SQL_C_SBIGINT;
                    output.sql_types[index] = SQL_BIGINT;
                    output.buffers[index] = &output.integers.back();
                }
                else if constexpr(std::same_as<current_type, std::uint64_t>)
                {
                    output.unsigned_integers.push_back(current);
                    output.c_types[index] = SQL_C_UBIGINT;
                    output.sql_types[index] = SQL_BIGINT;
                    output.buffers[index] = &output.unsigned_integers.back();
                }
                else if constexpr(std::same_as<current_type, double>)
                {
                    output.reals.push_back(current);
                    output.c_types[index] = SQL_C_DOUBLE;
                    output.sql_types[index] = SQL_DOUBLE;
                    output.buffers[index] = &output.reals.back();
                }
                else if constexpr(std::same_as<current_type, std::string>)
                {
                    output.texts.push_back(utf8_to_utf16(current));
                    output.c_types[index] = SQL_C_WCHAR;
                    output.sql_types[index] = SQL_WVARCHAR;
                    output.buffers[index] = output.texts.back().data();
                    output.lengths[index] = static_cast<SQLLEN>((output.texts.back().size() - 1) * sizeof(SQLWCHAR));
                    output.column_sizes[index] = static_cast<SQLULEN>(output.texts.back().size() - 1);
                    // For input strings the indicator carries the byte length.
                    output.indicators[index] = output.lengths[index];
                }
                else
                {
                    output.binaries.emplace_back(reinterpret_cast<const char*>(current.data()),
                                                 reinterpret_cast<const char*>(current.data()) + current.size());
                    output.c_types[index] = SQL_C_BINARY;
                    output.sql_types[index] = SQL_VARBINARY;
                    output.buffers[index] = output.binaries.back().data();
                    output.lengths[index] = static_cast<SQLLEN>(output.binaries.back().size());
                    output.column_sizes[index] = static_cast<SQLULEN>(output.binaries.back().size());
                    output.indicators[index] = output.lengths[index];
                }
            },
            values[index]
        );
    }

    return output;
}

[[nodiscard]] sql_value read_column(SQLHSTMT statement, SQLUSMALLINT column, SQLSMALLINT type)
{
    SQLLEN indicator = 0;

    switch(type)
    {
        case SQL_BIT:
        {
            unsigned char value = 0;
            if(SQLGetData(statement, column, SQL_C_BIT, &value, sizeof(value), &indicator) == SQL_NULL_DATA)
            {
                return std::monostate{};
            }

            return value != 0;
        }

        case SQL_TINYINT:
        case SQL_SMALLINT:
        case SQL_INTEGER:
        case SQL_BIGINT:
        {
            SQLBIGINT value = 0;
            if(SQLGetData(statement, column, SQL_C_SBIGINT, &value, sizeof(value), &indicator) == SQL_NULL_DATA)
            {
                return std::monostate{};
            }

            return static_cast<std::int64_t>(value);
        }

        case SQL_REAL:
        case SQL_FLOAT:
        case SQL_DOUBLE:
        case SQL_DECIMAL:
        case SQL_NUMERIC:
        {
            double value = 0.0;
            if(SQLGetData(statement, column, SQL_C_DOUBLE, &value, sizeof(value), &indicator) == SQL_NULL_DATA)
            {
                return std::monostate{};
            }

            return value;
        }

        case SQL_BINARY:
        case SQL_VARBINARY:
        case SQL_LONGVARBINARY:
        {
            std::vector<std::byte> output;
            char                   buffer[4096];

            while(true)
            {
                const auto result = SQLGetData(statement, column, SQL_C_BINARY, buffer, sizeof(buffer), &indicator);

                if(result == SQL_NULL_DATA)
                {
                    return std::monostate{};
                }

                if(result == SQL_NO_DATA)
                {
                    break;
                }

                if(!succeeded(result))
                {
                    break;
                }

                const auto chunk = std::min<std::size_t>(indicator, sizeof(buffer));
                for(std::size_t index = 0; index < chunk; ++index)
                {
                    output.push_back(static_cast<std::byte>(static_cast<unsigned char>(buffer[index])));
                }

                if(result != SQL_SUCCESS_WITH_INFO)
                {
                    break;
                }
            }

            return output;
        }

        default:
        {
            std::vector<SQLWCHAR> output;
            SQLWCHAR              buffer[2048];

            while(true)
            {
                const auto result = SQLGetData(statement, column, SQL_C_WCHAR, buffer, sizeof(buffer), &indicator);

                if(result == SQL_NULL_DATA)
                {
                    return std::monostate{};
                }

                if(result == SQL_NO_DATA)
                {
                    break;
                }

                if(!succeeded(result))
                {
                    break;
                }

                const auto characters = indicator > 0 ? static_cast<std::size_t>(indicator / static_cast<SQLLEN>(sizeof(SQLWCHAR)))
                                                      : std::size_t{0};
                output.insert(output.end(), buffer, buffer + characters);

                if(result != SQL_SUCCESS_WITH_INFO)
                {
                    break;
                }
            }

            return utf16_to_utf8(output.data(), output.size());
        }
    }
}

} // namespace

class sqlserver_backend final : public backend
{
public:
    explicit sqlserver_backend(std::string uri)
    {
        const auto options = parse_sqlserver_uri(uri);

        if(SQLAllocHandle(SQL_HANDLE_DBC, shared_environment().value, &connection_) != SQL_SUCCESS)
        {
            throw connection_error{"entityfy SQL Server ODBC connection handle allocation failed"};
        }

        connect(options);

        // SET options that affect parsing must reach the server on their own,
        // not through a prepared batch.
        execute_direct("SET QUOTED_IDENTIFIER ON");
        execute_direct("SET ANSI_NULLS ON");
        execute_direct("SET ANSI_PADDING ON");
        execute_direct("SET ANSI_WARNINGS ON");
    }

    sqlserver_backend(const sqlserver_backend&) = delete;
    sqlserver_backend& operator=(const sqlserver_backend&) = delete;

    ~sqlserver_backend() override
    {
        {
            const std::lock_guard lock{mutex_};
            clear_statement_cache_locked();
        }

        if(connection_ != SQL_NULL_HDBC)
        {
            SQLDisconnect(connection_);
            SQLFreeHandle(SQL_HANDLE_DBC, connection_);
        }
    }

    [[nodiscard]] dialect target_dialect() const noexcept override
    {
        return dialect::sqlserver;
    }

    execution_result execute(const entityfy::statement& input) override
    {
        const std::lock_guard lock{mutex_};

        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            // Statements without parameters go through the text protocol;
            // transaction control in particular cannot be prepared.
            std::unique_ptr<statement_lease> lease;
            SQLHSTMT                         statement_handle = SQL_NULL_HSTMT;

            if(input.binds.empty())
            {
                statement_handle = execute_text(input);
            }
            else
            {
                lease = std::make_unique<statement_lease>(*this, input);
                statement_handle = lease->get();
            }

            execution_result output;

            SQLSMALLINT column_count = 0;
            if(SQLNumResultCols(statement_handle, &column_count) == SQL_SUCCESS && column_count > 0)
            {
                std::uint64_t produced = 0;

                // OUTPUT INSERTED returns one row; SQL_SUCCESS_WITH_INFO is a
                // successful fetch (a truncated warning, for instance), so only
                // SQL_NO_DATA ends the loop. Every produced row keeps the last
                // key, which is the same row for a single-statement INSERT.
                while(true)
                {
                    const auto fetched = SQLFetch(statement_handle);
                    if(fetched == SQL_NO_DATA)
                    {
                        break;
                    }

                    if(!succeeded(fetched))
                    {
                        const auto detail = describe_sqlserver_error(SQL_HANDLE_STMT, statement_handle);
                        throw_sqlserver_error("execute", input.sql, detail);
                    }

                    SQLLEN indicator = 0;
                    SQLBIGINT value = 0;

                    if(succeeded(SQLGetData(statement_handle, 1, SQL_C_SBIGINT, &value, sizeof(value), &indicator)) &&
                       indicator != SQL_NULL_DATA)
                    {
                        output.last_insert_id = static_cast<std::int64_t>(value);
                    }

                    ++produced;
                }

                output.rows_affected = produced;
            }
            else
            {
                SQLLEN affected = 0;
                SQLRowCount(statement_handle, &affected);
                output.rows_affected = affected > 0 ? static_cast<std::uint64_t>(affected) : 0;
            }

            // A dropped cursor would otherwise stay open on a cached handle.
            SQLFreeStmt(statement_handle, SQL_CLOSE);

            if(!lease)
            {
                SQLFreeHandle(SQL_HANDLE_STMT, statement_handle);
            }

            return output;
        }
        catch(const sql_error&)
        {
            throw;
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("SQL Server", "execute", input.sql, error.what());
        }
    }

    [[nodiscard]] std::vector<row> query(const entityfy::statement& input) override
    {
        std::vector<row> rows;

        query_each(input, [&](const row& current) {
            rows.push_back(current);
            return true;
        });

        return rows;
    }

    void query_each(const entityfy::statement& input,
                    const std::function<bool(const row&)>& consumer) override
    {
        const std::lock_guard lock{mutex_};
        bool in_consumer = false;

        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            statement_lease lease{*this, input};
            SQLHSTMT        statement_handle = lease.get();

            SQLSMALLINT column_count = 0;
            if(SQLNumResultCols(statement_handle, &column_count) != SQL_SUCCESS)
            {
                throw std::runtime_error{"entityfy SQL Server query returned no result set"};
            }

            std::vector<SQLSMALLINT> types(column_count, SQL_VARCHAR);

            for(SQLSMALLINT index = 0; index < column_count; ++index)
            {
                SQLSMALLINT type = SQL_VARCHAR;
                SQLDescribeColW(statement_handle,
                                static_cast<SQLUSMALLINT>(index + 1),
                                nullptr,
                                0,
                                nullptr,
                                &type,
                                nullptr,
                                nullptr,
                                nullptr);
                types[index] = type;
            }

            row current;

            while(true)
            {
                const auto fetched = SQLFetch(statement_handle);
                if(fetched == SQL_NO_DATA)
                {
                    break;
                }

                if(!succeeded(fetched))
                {
                    const auto detail = describe_sqlserver_error(SQL_HANDLE_STMT, statement_handle);
                    throw_sqlserver_error("query", input.sql, detail);
                }

                current.clear();
                current.reserve(column_count);

                for(SQLSMALLINT index = 0; index < column_count; ++index)
                {
                    current.emplace_back(read_column(statement_handle, static_cast<SQLUSMALLINT>(index + 1), types[index]));
                }

                in_consumer = true;
                const bool keep = consumer(current);
                in_consumer = false;

                if(!keep)
                {
                    break;
                }
            }

            // The handle may go back to the statement cache, so an unfinished
            // cursor must be closed here rather than left for the next user.
            SQLFreeStmt(statement_handle, SQL_CLOSE);
        }
        catch(const sql_error&)
        {
            throw;
        }
        catch(const std::exception& error)
        {
            if(in_consumer)
            {
                throw;
            }

            throw_classified_sql_error("SQL Server", "query", input.sql, error.what());
        }
    }

    // Prepared statements are kept in an LRU keyed by SQL text. A cache hit
    // closes the previous cursor and resets the parameter bindings before the
    // statement is executed again.
    void set_statement_cache_capacity(std::size_t capacity) override
    {
        const std::lock_guard lock{mutex_};
        capacity_ = capacity;

        while(cache_.size() > capacity_)
        {
            SQLFreeHandle(SQL_HANDLE_STMT, cache_.back().second);
            index_.erase(cache_.back().first);
            cache_.pop_back();
        }
    }

    [[nodiscard]] std::size_t statement_cache_capacity() const noexcept override
    {
        const std::lock_guard lock{mutex_};
        return capacity_;
    }

    [[nodiscard]] std::size_t statement_cache_size() const noexcept override
    {
        const std::lock_guard lock{mutex_};
        return cache_.size();
    }

    void clear_statement_cache() override
    {
        const std::lock_guard lock{mutex_};
        clear_statement_cache_locked();
    }

    [[nodiscard]] std::vector<std::string> table_columns(std::string_view table) override
    {
        statement input;
        input.sql =
            "SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS "
            "WHERE TABLE_NAME = ? AND TABLE_SCHEMA = SCHEMA_NAME() ORDER BY ORDINAL_POSITION";
        input.binds.emplace_back(std::string{table});

        std::vector<std::string> output;
        for(const auto& result_row: query(input))
        {
            if(!result_row.empty())
            {
                output.emplace_back(value_to_string(result_row.front()));
            }
        }

        return output;
    }

    [[nodiscard]] table_info inspect_table(std::string_view table) override
    {
        table_info output;
        output.name = std::string{table};

        statement columns_statement;
        columns_statement.sql =
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
            "WHERE TABLE_NAME = ? AND TABLE_SCHEMA = SCHEMA_NAME() ORDER BY ORDINAL_POSITION";
        columns_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(columns_statement))
        {
            if(result_row.size() < 5)
            {
                continue;
            }

            output.columns.emplace_back(column_info{
                .name = value_to_string(result_row[0]),
                .sql_type = value_to_string(result_row[1]),
                .nullable = value_to_string(result_row[2]) == "YES",
                .primary_key = false,
                .default_sql = value_to_string(result_row[3]),
            });
        }

        statement primary_key_statement;
        primary_key_statement.sql =
            "SELECT k.COLUMN_NAME FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS t "
            "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE k ON k.CONSTRAINT_NAME = t.CONSTRAINT_NAME "
            "WHERE t.CONSTRAINT_TYPE = 'PRIMARY KEY' AND k.TABLE_NAME = ? AND k.TABLE_SCHEMA = SCHEMA_NAME()";
        primary_key_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(primary_key_statement))
        {
            if(result_row.empty())
            {
                continue;
            }

            const auto name = value_to_string(result_row.front());
            for(auto& column : output.columns)
            {
                if(column.name == name)
                {
                    column.primary_key = true;
                }
            }
        }

        statement indexes_statement;
        indexes_statement.sql =
            "SELECT i.name, i.is_unique, c.name FROM sys.indexes i "
            "JOIN sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id "
            "JOIN sys.columns c ON c.object_id = ic.object_id AND c.column_id = ic.column_id "
            "WHERE i.object_id = OBJECT_ID(?) AND i.is_primary_key = 0 "
            "ORDER BY i.name, ic.key_ordinal";
        indexes_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(indexes_statement))
        {
            if(result_row.size() < 3)
            {
                continue;
            }

            const auto name = value_to_string(result_row[0]);
            const bool unique = value_to_string(result_row[1]) == "1";
            const auto column = value_to_string(result_row[2]);

            const auto existing = std::find_if(output.indexes.begin(), output.indexes.end(), [&](const index_info& index) {
                return index.name == name;
            });

            if(existing == output.indexes.end())
            {
                output.indexes.emplace_back(index_info{.name = name, .unique = unique, .columns = {column}});
            }
            else
            {
                existing->columns.emplace_back(column);
            }
        }

        statement foreign_keys_statement;
        foreign_keys_statement.sql =
            "SELECT k.COLUMN_NAME, ccu.TABLE_NAME, ccu.COLUMN_NAME, rc.UPDATE_RULE, rc.DELETE_RULE "
            "FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE k "
            "JOIN INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS rc ON rc.CONSTRAINT_NAME = k.CONSTRAINT_NAME "
            "JOIN INFORMATION_SCHEMA.CONSTRAINT_COLUMN_USAGE ccu "
            "ON ccu.CONSTRAINT_NAME = rc.UNIQUE_CONSTRAINT_NAME "
            "WHERE k.TABLE_NAME = ? AND k.TABLE_SCHEMA = SCHEMA_NAME()";
        foreign_keys_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(foreign_keys_statement))
        {
            if(result_row.size() < 5)
            {
                continue;
            }

            output.foreign_keys.emplace_back(foreign_key_info{
                .column = value_to_string(result_row[0]),
                .referenced_table = value_to_string(result_row[1]),
                .referenced_column = value_to_string(result_row[2]),
                .on_update = value_to_string(result_row[3]),
                .on_delete = value_to_string(result_row[4]),
            });
        }

        return output;
    }

    void set_batch_parameter_limit(std::size_t limit) override
    {
        std::lock_guard lock{mutex_};
        batch_parameter_limit_ = limit == 0 ? 1 : limit;
    }

    [[nodiscard]] std::size_t batch_parameter_limit() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return batch_parameter_limit_;
    }

    void set_logger(std::function<void(std::string_view)> logger) override
    {
        std::lock_guard lock{mutex_};
        logger_ = std::move(logger);
    }

private:
    void connect(const sqlserver_uri_options& options)
    {
        const auto connection_string = sqlserver_connection_string(options);
        auto       wide = utf8_to_utf16(connection_string);
        SQLWCHAR   output[1024]{};
        SQLSMALLINT output_length = 0;

        if(!succeeded(SQLDriverConnectW(
               connection_,
               nullptr,
               wide.data(),
               SQL_NTS,
               output,
               static_cast<SQLSMALLINT>(std::size(output)),
               &output_length,
               SQL_DRIVER_NOPROMPT
           )))
        {
            const auto detail = describe_sqlserver_error(SQL_HANDLE_DBC, connection_);

            // A failed login or an unreachable server is a connection problem,
            // never a statement problem, so it has no SQL text to carry.
            throw connection_error{"entityfy SQL Server connection failed: " + detail.message};
        }
    }

    void execute_direct(std::string_view sql)
    {
        SQLHSTMT statement_handle = SQL_NULL_HSTMT;
        if(SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_handle) != SQL_SUCCESS)
        {
            throw std::runtime_error{"entityfy SQL Server statement allocation failed"};
        }

        auto wide = utf8_to_utf16(sql);
        const auto result = SQLExecDirectW(statement_handle, wide.data(), SQL_NTS);

        if(!succeeded(result))
        {
            // The diagnostics live on the statement handle, so read them before
            // it is freed.
            const auto detail = describe_sqlserver_error(SQL_HANDLE_STMT, statement_handle);
            SQLFreeHandle(SQL_HANDLE_STMT, statement_handle);
            throw_sqlserver_error("execute", std::string{sql}, detail);
        }

        SQLFreeHandle(SQL_HANDLE_STMT, statement_handle);
    }

    [[nodiscard]] SQLHSTMT execute_text(const statement& input)
    {
        SQLHSTMT statement_handle = SQL_NULL_HSTMT;
        if(SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_handle) != SQL_SUCCESS)
        {
            throw std::runtime_error{"entityfy SQL Server statement allocation failed"};
        }

        auto wide = utf8_to_utf16(input.sql);

        if(!succeeded(SQLExecDirectW(statement_handle, wide.data(), SQL_NTS)))
        {
            const auto detail = describe_sqlserver_error(SQL_HANDLE_STMT, statement_handle);
            SQLFreeHandle(SQL_HANDLE_STMT, statement_handle);
            throw_sqlserver_error("execute", input.sql, detail);
        }

        return statement_handle;
    }

    struct acquired_statement
    {
        SQLHSTMT handle = SQL_NULL_HSTMT;
        bool     reused = false;
    };

    class statement_lease
    {
    public:
        statement_lease(sqlserver_backend& owner, const statement& input)
            : owner_{owner}, sql_{input.sql}
        {
            const auto acquired = owner_.acquire_statement_locked(input);
            handle_ = acquired.handle;
            reused_ = acquired.reused;
        }

        statement_lease(const statement_lease&) = delete;
        statement_lease& operator=(const statement_lease&) = delete;

        ~statement_lease()
        {
            owner_.release_statement_locked(sql_, handle_, reused_);
        }

        [[nodiscard]] SQLHSTMT get() const noexcept
        {
            return handle_;
        }

    private:
        sqlserver_backend& owner_;
        std::string        sql_;
        SQLHSTMT           handle_ = SQL_NULL_HSTMT;
        bool               reused_ = false;
    };

    [[nodiscard]] acquired_statement acquire_statement_locked(const statement& input)
    {
        SQLHSTMT statement_handle = SQL_NULL_HSTMT;
        bool     reused = false;

        if(capacity_ != 0)
        {
            const auto found = index_.find(input.sql);

            if(found != index_.end())
            {
                statement_handle = found->second->second;
                SQLFreeStmt(statement_handle, SQL_CLOSE);
                SQLFreeStmt(statement_handle, SQL_RESET_PARAMS);
                cache_.splice(cache_.begin(), cache_, found->second);
                reused = true;
            }
        }

        if(statement_handle == SQL_NULL_HSTMT)
        {
            if(SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_handle) != SQL_SUCCESS)
            {
                throw std::runtime_error{"entityfy SQL Server statement allocation failed"};
            }

            auto wide = utf8_to_utf16(input.sql);

            if(!succeeded(SQLPrepareW(statement_handle, wide.data(), SQL_NTS)))
            {
                const auto detail = describe_sqlserver_error(SQL_HANDLE_STMT, statement_handle);
                SQLFreeHandle(SQL_HANDLE_STMT, statement_handle);
                throw_sqlserver_error("prepare", input.sql, detail);
            }
        }

        // The bind buffers must stay alive until SQLExecute returns.
        auto parameters = make_sqlserver_parameters(input.binds);

        for(std::size_t index = 0; index < input.binds.size(); ++index)
        {
            if(!succeeded(SQLBindParameter(
                   statement_handle,
                   static_cast<SQLUSMALLINT>(index + 1),
                   SQL_PARAM_INPUT,
                   parameters.c_types[index],
                   parameters.sql_types[index],
                   parameters.column_sizes[index],
                   parameters.decimal_digits[index],
                   parameters.buffers[index],
                   parameters.lengths[index],
                   &parameters.indicators[index]
               )))
            {
                const auto detail = describe_sqlserver_error(SQL_HANDLE_STMT, statement_handle);
                discard_statement_locked(input.sql, statement_handle, reused);
                throw_sqlserver_error("bind", input.sql, detail);
            }
        }

        if(!succeeded(SQLExecute(statement_handle)))
        {
            const auto detail = describe_sqlserver_error(SQL_HANDLE_STMT, statement_handle);
            discard_statement_locked(input.sql, statement_handle, reused);
            throw_sqlserver_error("execute", input.sql, detail);
        }

        return acquired_statement{.handle = statement_handle, .reused = reused};
    }

    void release_statement_locked(std::string_view sql, SQLHSTMT handle, bool reused)
    {
        if(reused)
        {
            return;
        }

        if(capacity_ == 0)
        {
            SQLFreeHandle(SQL_HANDLE_STMT, handle);
            return;
        }

        while(cache_.size() >= capacity_)
        {
            SQLFreeHandle(SQL_HANDLE_STMT, cache_.back().second);
            index_.erase(cache_.back().first);
            cache_.pop_back();
        }

        cache_.emplace_front(std::string{sql}, handle);
        index_[std::string{sql}] = cache_.begin();
    }

    void discard_statement_locked(std::string_view sql, SQLHSTMT handle, bool reused)
    {
        if(reused)
        {
            index_.erase(std::string{sql});
            cache_.remove_if([&](const auto& entry) { return entry.second == handle; });
        }

        SQLFreeHandle(SQL_HANDLE_STMT, handle);
    }

    void clear_statement_cache_locked() noexcept
    {
        for(const auto& entry: cache_)
        {
            SQLFreeHandle(SQL_HANDLE_STMT, entry.second);
        }

        cache_.clear();
        index_.clear();
    }

    SQLHDBC                               connection_ = SQL_NULL_HDBC;
    mutable std::mutex                    mutex_;
    std::size_t                           batch_parameter_limit_ = 2000;
    std::size_t                           capacity_ = 32;
    std::list<std::pair<std::string, SQLHSTMT>> cache_;
    std::unordered_map<std::string, decltype(cache_)::iterator> index_;
    std::function<void(std::string_view)> logger_;
};

std::unique_ptr<backend> make_sqlserver_backend(std::string uri)
{
    return std::make_unique<sqlserver_backend>(std::move(uri));
}

} // namespace entityfy::detail
