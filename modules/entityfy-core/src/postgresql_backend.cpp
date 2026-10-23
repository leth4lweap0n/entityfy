#include <entityfy/detail/backend.hpp>
#include <entityfy/error.hpp>
#include <entityfy/statement.hpp>

#include <libpq-fe.h>

#include <atomic>
#include <charconv>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace entityfy::detail
{
namespace
{

constexpr std::size_t max_pg_parameters = 1024;

// pg_type.oid for bytea; libpq does not expose the catalog constants.
constexpr Oid bytea_oid = 17;

struct pg_connection_deleter
{
    void operator()(PGconn* connection) const noexcept
    {
        if(connection != nullptr)
        {
            PQfinish(connection);
        }
    }
};

struct pg_result_deleter
{
    void operator()(PGresult* result) const noexcept
    {
        if(result != nullptr)
        {
            PQclear(result);
        }
    }
};

struct pg_bytea_deleter
{
    void operator()(unsigned char* buffer) const noexcept
    {
        if(buffer != nullptr)
        {
            PQfreemem(buffer);
        }
    }
};

using pg_connection_ptr = std::unique_ptr<PGconn, pg_connection_deleter>;
using pg_result_ptr     = std::unique_ptr<PGresult, pg_result_deleter>;
using pg_bytea_ptr      = std::unique_ptr<unsigned char, pg_bytea_deleter>;

// The PGresult already carries the SQLSTATE and the primary message; both are
// copied out before the result is released so a failure can still be
// classified after a pipeline has been drained.
struct pg_error_info
{
    std::string sqlstate;
    std::string message;
};

[[nodiscard]] pg_error_info result_error_info(PGresult* result)
{
    const char* sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
    const char* message = PQresultErrorMessage(result);

    return pg_error_info{
        .sqlstate = sqlstate != nullptr ? sqlstate : "",
        .message = message != nullptr ? message : "",
    };
}

[[nodiscard]] std::string connection_message(PGconn* connection)
{
    const char* message = PQerrorMessage(connection);
    return message != nullptr ? std::string{message} : std::string{};
}

// The reactor never waits longer than this for socket readiness, so a cancel
// request, a shutdown or a dead server is noticed within one poll interval.
constexpr pg_usec_time_t async_poll_interval_us = 50'000;

// Bound on a single cancel request: a cancel connection that cannot deliver in
// this window is abandoned and the statement simply runs to completion.
constexpr pg_usec_time_t async_cancel_budget_us = 1'000'000;

// SQLSTATE for a prepared statement name the server does not know. It is
// raised before anything executes, so the statement can be re-sent plain.
constexpr std::string_view sqlstate_invalid_statement_name = "26000";

// The three statuses that mean the server switched the connection into the
// COPY protocol. entityfy has no copy API, and libpq answers every PQgetResult
// in this state with a fresh COPY result instead of the terminating null, so a
// copy has to be recognised explicitly or a drain never finishes.
[[nodiscard]] bool is_copy_status(ExecStatusType status) noexcept
{
    return status == PGRES_COPY_IN || status == PGRES_COPY_OUT || status == PGRES_COPY_BOTH;
}

// Raised by the async machinery when the server aborted the statement because
// of our own cancel request. It never escapes the job runner, which turns it
// into task_completer::set_cancelled().
class async_cancelled : public std::exception
{
public:
    [[nodiscard]] const char* what() const noexcept override
    {
        return "async PostgreSQL operation was cancelled";
    }
};

// The one cancel request an async operation may deliver. A 57014 is only
// attributed to us when the request really reached the server; a server side
// abort on its own (statement_timeout, ...) must keep its classification.
struct cancel_state
{
    bool requested = false;
    bool delivered = false;
};

// libpq's nonblocking mode for exactly one async operation. The synchronous
// path calls the blocking API, so the mode is restored on every exit path.
class nonblocking_scope
{
public:
    explicit nonblocking_scope(PGconn* connection)
        : connection_{connection}
    {
        if(PQsetnonblocking(connection_, 1) != 0)
        {
            throw std::runtime_error{
                "unable to switch the PostgreSQL connection to nonblocking mode: " + connection_message(connection_)
            };
        }
    }

    nonblocking_scope(const nonblocking_scope&) = delete;
    nonblocking_scope& operator=(const nonblocking_scope&) = delete;

    ~nonblocking_scope()
    {
        (void)PQsetnonblocking(connection_, 0);
    }

private:
    PGconn* connection_;
};

// Publishes the end of the single in-flight operation while the reactor still
// holds the connection mutex, so a synchronous call that finds the connection
// unlocked also observes the flag clear and never rejects a connection that is
// already free.
class flight_release
{
public:
    explicit flight_release(std::atomic<bool>& busy) noexcept
        : busy_{busy}
    {
    }

    flight_release(const flight_release&) = delete;
    flight_release& operator=(const flight_release&) = delete;

    ~flight_release()
    {
        busy_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool>& busy_;
};

// The two SQLSTATEs a retry can resolve get a dedicated entityfy type,
// everything else keeps the message based classification.
[[noreturn]] void throw_pg_error(std::string_view operation,
                                 const std::string& sql,
                                 std::string_view sqlstate,
                                 std::string_view message)
{
    auto text = sql_context_message("PostgreSQL", operation, sql, message);

    if(sqlstate == "40001")
    {
        throw serialization_failure{std::move(text), sql};
    }

    if(sqlstate == "40P01")
    {
        throw deadlock_detected{std::move(text), sql};
    }

    throw_classified_sql_error("PostgreSQL", operation, sql, message);
}

// A failed PGresult; carries the SQLSTATE so execute/query can classify it
// without parsing the message.
class pg_statement_error : public std::runtime_error
{
public:
    pg_statement_error(std::string message, std::string sqlstate)
        : std::runtime_error{std::move(message)},
          sqlstate_{std::move(sqlstate)}
    {
    }

    [[nodiscard]] const std::string& sqlstate() const noexcept
    {
        return sqlstate_;
    }

private:
    std::string sqlstate_;
};

struct pg_parameter
{
    bool        null = false;
    std::string text;
};

[[nodiscard]] pg_parameter to_pg_parameter(const sql_value& value)
{
    return std::visit(
        [](const auto& current) -> pg_parameter {
            using current_type = std::remove_cvref_t<decltype(current)>;

            if constexpr(std::same_as<current_type, std::monostate>)
            {
                return pg_parameter{.null = true, .text = {}};
            }
            else if constexpr(std::same_as<current_type, bool>)
            {
                return pg_parameter{.null = false, .text = current ? "true" : "false"};
            }
            else if constexpr(std::same_as<current_type, std::string>)
            {
                return pg_parameter{.null = false, .text = current};
            }
            else if constexpr(std::same_as<current_type, bytes>)
            {
                std::string text = "\\x";
                constexpr char hex[] = "0123456789abcdef";
                text.reserve(2 + current.size() * 2);

                for(const auto byte: current)
                {
                    const auto value = static_cast<unsigned char>(byte);
                    text.push_back(hex[value >> 4U]);
                    text.push_back(hex[value & 0x0FU]);
                }

                return pg_parameter{.null = false, .text = std::move(text)};
            }
            else
            {
                return pg_parameter{.null = false, .text = std::to_string(current)};
            }
        },
        value
    );
}

[[nodiscard]] bytes decode_bytea(const char* value)
{
    std::size_t  size = 0;
    pg_bytea_ptr decoded{PQunescapeBytea(reinterpret_cast<const unsigned char*>(value), &size)};

    if(decoded == nullptr)
    {
        throw std::invalid_argument{"invalid bytea value returned by PostgreSQL"};
    }

    bytes output(size);
    if(size != 0)
    {
        std::memcpy(output.data(), decoded.get(), size);
    }

    return output;
}

} // namespace

[[nodiscard]] std::string postgres_placeholders(std::string_view sql)
{
    std::string output;
    output.reserve(sql.size());

    enum class state
    {
        normal,
        single_quoted,
        double_quoted,
        line_comment,
        block_comment,
        dollar_quoted,
    };

    const auto is_dollar_tag_character = [](char character) {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') ||
               character == '_';
    };

    const auto dollar_tag_at = [&](std::size_t position) -> std::string {
        if(sql[position] != '$')
        {
            return {};
        }

        std::size_t end = position + 1;
        while(end < sql.size() && is_dollar_tag_character(sql[end]))
        {
            ++end;
        }

        if(end < sql.size() && sql[end] == '$')
        {
            return std::string{sql.substr(position, end - position + 1)};
        }

        return {};
    };

    std::size_t placeholder_index = 1;
    state       parser_state = state::normal;
    std::string dollar_tag;

    for(std::size_t index = 0; index < sql.size(); ++index)
    {
        const char character = sql[index];

        if(parser_state == state::single_quoted)
        {
            output.push_back(character);
            if(character == '\'' && index + 1 < sql.size() && sql[index + 1] == '\'')
            {
                output.push_back(sql[++index]);
            }
            else if(character == '\'')
            {
                parser_state = state::normal;
            }

            continue;
        }

        if(parser_state == state::double_quoted)
        {
            output.push_back(character);
            if(character == '"' && index + 1 < sql.size() && sql[index + 1] == '"')
            {
                output.push_back(sql[++index]);
            }
            else if(character == '"')
            {
                parser_state = state::normal;
            }

            continue;
        }

        if(parser_state == state::line_comment)
        {
            output.push_back(character);
            if(character == '\n')
            {
                parser_state = state::normal;
            }

            continue;
        }

        if(parser_state == state::block_comment)
        {
            output.push_back(character);
            if(character == '*' && index + 1 < sql.size() && sql[index + 1] == '/')
            {
                output.push_back(sql[++index]);
                parser_state = state::normal;
            }

            continue;
        }

        if(parser_state == state::dollar_quoted)
        {
            if(sql.substr(index, dollar_tag.size()) == std::string_view{dollar_tag})
            {
                output.append(dollar_tag);
                index += dollar_tag.size() - 1;
                dollar_tag.clear();
                parser_state = state::normal;
            }
            else
            {
                output.push_back(character);
            }

            continue;
        }

        if(character == '?')
        {
            output.push_back('$');
            output.append(std::to_string(placeholder_index++));
        }
        else if(character == '\'')
        {
            output.push_back(character);
            parser_state = state::single_quoted;
        }
        else if(character == '"')
        {
            output.push_back(character);
            parser_state = state::double_quoted;
        }
        else if(character == '-' && index + 1 < sql.size() && sql[index + 1] == '-')
        {
            output.push_back(character);
            output.push_back(sql[++index]);
            parser_state = state::line_comment;
        }
        else if(character == '/' && index + 1 < sql.size() && sql[index + 1] == '*')
        {
            output.push_back(character);
            output.push_back(sql[++index]);
            parser_state = state::block_comment;
        }
        else if(character == '$')
        {
            auto tag = dollar_tag_at(index);
            if(tag.empty())
            {
                if(index + 1 < sql.size() && sql[index + 1] >= '1' && sql[index + 1] <= '9')
                {
                    return std::string{sql};
                }

                output.push_back(character);
            }
            else
            {
                output.append(tag);
                index += tag.size() - 1;
                dollar_tag = std::move(tag);
                parser_state = state::dollar_quoted;
            }
        }
        else
        {
            output.push_back(character);
        }
    }

    return output;
}

[[nodiscard]] sql_value postgresql_text_value_to_sql_value(const char* value, Oid type_oid)
{
    if(type_oid == bytea_oid)
    {
        return decode_bytea(value);
    }

    return std::string{value};
}

namespace
{

[[nodiscard]] std::vector<pg_parameter> to_pg_parameters(const std::vector<sql_value>& values)
{
    std::vector<pg_parameter> output;
    output.reserve(values.size());

    for(const auto& value: values)
    {
        output.emplace_back(to_pg_parameter(value));
    }

    return output;
}

// Arrays handed to PQexecParams/PQsendQueryParams: text format for every
// parameter, no explicit type OIDs (the server infers them) and nullptr for a
// monostate value.
class pg_parameters
{
public:
    explicit pg_parameters(const std::vector<pg_parameter>& parameters)
    {
        if(parameters.size() > max_pg_parameters)
        {
            throw std::invalid_argument{"entityfy PostgreSQL backend supports at most 1024 bound parameters per statement"};
        }

        values_.reserve(parameters.size());
        lengths_.reserve(parameters.size());
        formats_.reserve(parameters.size());

        for(const auto& parameter: parameters)
        {
            values_.push_back(parameter.null ? nullptr : parameter.text.c_str());
            lengths_.push_back(parameter.null ? 0 : static_cast<int>(parameter.text.size()));
            formats_.push_back(0);
        }
    }

    [[nodiscard]] int count() const noexcept
    {
        return static_cast<int>(values_.size());
    }

    [[nodiscard]] const char* const* values() const noexcept
    {
        return values_.data();
    }

    [[nodiscard]] const int* lengths() const noexcept
    {
        return lengths_.data();
    }

    [[nodiscard]] const int* formats() const noexcept
    {
        return formats_.data();
    }

private:
    std::vector<const char*> values_;
    std::vector<int>        lengths_;
    std::vector<int>        formats_;
};

[[nodiscard]] std::uint64_t rows_affected(PGresult* result)
{
    const char* tuples = PQcmdTuples(result);
    if(tuples == nullptr || tuples[0] == '\0')
    {
        return 0;
    }

    std::uint64_t value = 0;
    const auto*   end = tuples + std::strlen(tuples);
    std::from_chars(tuples, end, value);
    return value;
}

void append_row(PGresult* result, int row_index, row& output)
{
    const int columns = PQnfields(result);
    output.clear();
    output.reserve(static_cast<std::size_t>(columns));

    for(int column = 0; column < columns; ++column)
    {
        if(PQgetisnull(result, row_index, column) != 0)
        {
            output.emplace_back(std::monostate{});
        }
        else
        {
            output.emplace_back(postgresql_text_value_to_sql_value(
                PQgetvalue(result, row_index, column),
                PQftype(result, column)
            ));
        }
    }
}

} // namespace

class postgresql_backend final : public backend
{
public:
    explicit postgresql_backend(std::string connection_info)
        : connection_{PQconnectdb(connection_info.c_str())}
    {
        if(connection_ == nullptr)
        {
            throw connection_error{"entityfy PostgreSQL connection could not be established: libpq could not allocate a connection"};
        }

        if(PQstatus(connection_.get()) != CONNECTION_OK)
        {
            throw connection_error{
                "entityfy PostgreSQL connection could not be established: " + connection_message(connection_.get())
            };
        }
    }

    postgresql_backend(const postgresql_backend&) = delete;
    postgresql_backend& operator=(const postgresql_backend&) = delete;

    // Stops the reactor after it has drained its queue: a job that is already
    // running completes its task before the connection is closed, so no task is
    // left pending by the destruction. Destroying the backend from a completion
    // callback would join the calling thread, so that case is diagnosed and
    // aborted instead of ending in an unexplained std::terminate.
    ~postgresql_backend() override
    {
        {
            const std::lock_guard lock{queue_mutex_};

            if(reactor_.joinable() && reactor_id_ == std::this_thread::get_id())
            {
                std::fputs(
                    "entityfy: PostgreSQL backend destroyed on its own reactor thread; a completion callback "
                    "must not release the last client. Move the value out and destroy it on the owning thread.\n",
                    stderr
                );
                std::abort();
            }

            stopping_ = true;
        }

        queue_cv_.notify_all();

        if(reactor_.joinable())
        {
            reactor_.join();
        }
    }

    [[nodiscard]] dialect target_dialect() const noexcept override
    {
        return dialect::postgresql;
    }

    execution_result execute(const entityfy::statement& input) override
    {
        require_idle_connection();
        const std::lock_guard lock{mutex_};
        require_idle_connection();

        return execute_locked(input);
    }

    // Batched writes use libpq's pipeline mode: every statement is sent before
    // any result is awaited, so a multi-chunk insert costs one round trip.
    [[nodiscard]] std::vector<execution_result> execute_batch(
        const std::vector<entityfy::statement>& statements) override
    {
        require_idle_connection();
        const std::lock_guard lock{mutex_};
        require_idle_connection();

        bool pipeline_ready = statements.size() > 1;

        for(const auto& entry: statements)
        {
            const auto sql = postgres_placeholders(entry.sql);

            if(is_ddl(sql) || !is_preparable(sql))
            {
                pipeline_ready = false;
                break;
            }
        }

        if(!pipeline_ready)
        {
            std::vector<execution_result> output;
            output.reserve(statements.size());

            for(const auto& entry: statements)
            {
                output.push_back(execute_locked(entry));
            }

            return output;
        }

        try
        {
            std::vector<std::string> sqls;
            sqls.reserve(statements.size());

            for(const auto& entry: statements)
            {
                sqls.push_back(postgres_placeholders(entry.sql));
            }

            // Prepared plans are used only when every distinct text fits in the
            // cache: preparing more would evict an earlier chunk's plan and the
            // pipeline would then send a name the server no longer knows.
            std::vector<std::string> prepared_names(statements.size());
            bool use_prepared = capacity_ != 0;

            if(use_prepared)
            {
                std::set<std::string> distinct{sqls.begin(), sqls.end()};
                use_prepared = distinct.size() <= capacity_;

                if(use_prepared)
                {
                    for(std::size_t index = 0; index < statements.size(); ++index)
                    {
                        prepared_names[index] = statement_name_for(sqls[index]);
                    }
                }
            }

            return run_pipeline(statements, sqls, prepared_names);
        }
        catch(const pg_statement_error& error)
        {
            throw_pg_error("execute batch", statements.front().sql, error.sqlstate(), error.what());
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("PostgreSQL", "execute batch", statements.front().sql, error.what());
        }
    }

    // ---------------------------------------------------------------------
    // Native asynchronous execution
    //
    // The connection itself is driven in libpq's nonblocking mode on a single
    // reactor thread; nothing is offloaded to another thread while a statement
    // waits for the server. One operation at a time may be in flight, because a
    // PGconn carries exactly one command stream.
    // ---------------------------------------------------------------------
    [[nodiscard]] async_capability async_mode() const noexcept override
    {
        return async_capability::native;
    }

    entityfy::task<execution_result> execute_async(const entityfy::statement& input) override
    {
        return submit<execution_result>([this, input](const std::shared_ptr<std::atomic<bool>>& cancel) {
            return execute_async_locked(input, cancel);
        });
    }

    entityfy::task<std::vector<row>> query_async(const entityfy::statement& input) override
    {
        return submit<std::vector<row>>([this, input](const std::shared_ptr<std::atomic<bool>>& cancel) {
            return query_async_locked(input, cancel);
        });
    }

    entityfy::task<std::vector<execution_result>> execute_batch_async(
        const std::vector<entityfy::statement>& statements) override
    {
        return submit<std::vector<execution_result>>(
            [this, statements](const std::shared_ptr<std::atomic<bool>>& cancel) {
                return execute_batch_async_locked(statements, cancel);
            }
        );
    }

private:
    execution_result execute_locked(const entityfy::statement& input)
    {
        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            auto sql = postgres_placeholders(input.sql);
            auto parameters = to_pg_parameters(input.binds);
            const pg_parameters bound{parameters};

            const bool ddl = is_ddl(sql);
            const std::string prepared = statement_name_for(sql);
            auto result = run_statement(sql, prepared, bound);

            // A prepared plan captures the schema at prepare time; after DDL the
            // cached plans may refer to a dropped or altered relation.
            if(ddl)
            {
                clear_statement_cache_locked();
            }

            return execution_result{.rows_affected = rows_affected(result.get())};
        }
        catch(const pg_statement_error& error)
        {
            throw_pg_error("execute", input.sql, error.sqlstate(), error.what());
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("PostgreSQL", "execute", input.sql, error.what());
        }
    }

public:
    [[nodiscard]] std::vector<row> query(const entityfy::statement& input) override
    {
        require_idle_connection();
        const std::lock_guard lock{mutex_};
        require_idle_connection();

        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            auto sql = postgres_placeholders(input.sql);
            auto parameters = to_pg_parameters(input.binds);
            const pg_parameters bound{parameters};

            const std::string prepared = statement_name_for(sql);
            auto result = run_statement(sql, prepared, bound);

            std::vector<row> rows;
            const int        count = PQntuples(result.get());
            rows.reserve(static_cast<std::size_t>(count));

            for(int index = 0; index < count; ++index)
            {
                row output;
                append_row(result.get(), index, output);
                rows.emplace_back(std::move(output));
            }

            return rows;
        }
        catch(const pg_statement_error& error)
        {
            throw_pg_error("query", input.sql, error.sqlstate(), error.what());
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("PostgreSQL", "query", input.sql, error.what());
        }
    }

    void query_each(const entityfy::statement& input,
                    const std::function<bool(const row&)>& consumer) override
    {
        require_idle_connection();
        const std::lock_guard lock{mutex_};
        require_idle_connection();
        bool in_consumer = false;

        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            auto sql = postgres_placeholders(input.sql);
            auto parameters = to_pg_parameters(input.binds);
            const pg_parameters bound{parameters};

            const std::string prepared = statement_name_for(sql);
            auto result = run_statement(sql, prepared, bound);

            const int count = PQntuples(result.get());
            row       output;

            for(int index = 0; index < count; ++index)
            {
                append_row(result.get(), index, output);

                in_consumer = true;
                const bool keep = consumer(output);
                in_consumer = false;
                if(!keep)
                {
                    break;
                }
            }
        }
        catch(const std::exception& error)
        {
            if(in_consumer)
            {
                throw;
            }

            throw_classified_sql_error("PostgreSQL", "query", input.sql, error.what());
        }
    }

    [[nodiscard]] std::vector<std::string> table_columns(std::string_view table) override
    {
        statement input;
        input.sql =
            "SELECT column_name FROM information_schema.columns "
            "WHERE table_schema = current_schema() AND table_name = ? "
            "ORDER BY ordinal_position";
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
            "SELECT column_name, data_type, is_nullable, column_default "
            "FROM information_schema.columns "
            "WHERE table_schema = current_schema() AND table_name = ? "
            "ORDER BY ordinal_position";
        columns_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(columns_statement))
        {
            if(result_row.size() < 4)
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
            "SELECT kcu.column_name "
            "FROM information_schema.table_constraints tc "
            "JOIN information_schema.key_column_usage kcu "
            "ON tc.constraint_name = kcu.constraint_name AND tc.table_schema = kcu.table_schema "
            "WHERE tc.constraint_type = 'PRIMARY KEY' "
            "AND tc.table_schema = current_schema() AND tc.table_name = ?";
        primary_key_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(primary_key_statement))
        {
            if(result_row.empty())
            {
                continue;
            }

            const auto primary_key_name = value_to_string(result_row.front());
            for(auto& column: output.columns)
            {
                if(column.name == primary_key_name)
                {
                    column.primary_key = true;
                }
            }
        }

        // The ordered key set lives on the primary key index: indkey lists the
        // key attributes in key order, so unnest with ordinality gives the
        // position of every key column.
        statement primary_key_order_statement;
        primary_key_order_statement.sql =
            "SELECT a.attname, keys.ordinality "
            "FROM pg_class t "
            "JOIN pg_namespace n ON n.oid = t.relnamespace "
            "JOIN pg_index ix ON t.oid = ix.indrelid AND ix.indisprimary "
            "JOIN unnest(ix.indkey) WITH ORDINALITY AS keys(attnum, ordinality) ON true "
            "JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = keys.attnum "
            "WHERE n.nspname = current_schema() AND t.relname = ? "
            "ORDER BY keys.ordinality";
        primary_key_order_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(primary_key_order_statement))
        {
            if(result_row.size() < 2)
            {
                continue;
            }

            const auto primary_key_name = value_to_string(result_row[0]);
            const auto primary_key_ordinal = static_cast<int>(value_to_i64(result_row[1]));

            for(auto& column: output.columns)
            {
                if(column.name == primary_key_name)
                {
                    column.primary_key_ordinal = primary_key_ordinal;
                }
            }
        }

        statement indexes_statement;
        indexes_statement.sql =
            "SELECT i.relname, ix.indisunique, string_agg(a.attname, ',' ORDER BY keys.ordinality) "
            "FROM pg_class t "
            "JOIN pg_namespace n ON n.oid = t.relnamespace "
            "JOIN pg_index ix ON t.oid = ix.indrelid "
            "JOIN pg_class i ON i.oid = ix.indexrelid "
            "JOIN unnest(ix.indkey) WITH ORDINALITY AS keys(attnum, ordinality) ON true "
            "JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = keys.attnum "
            "WHERE n.nspname = current_schema() AND t.relname = ? "
            "GROUP BY i.relname, ix.indisunique";
        indexes_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(indexes_statement))
        {
            if(result_row.size() < 3)
            {
                continue;
            }

            index_info index{
                .name = value_to_string(result_row[0]),
                .unique = value_to_string(result_row[1]) == "t" || value_to_string(result_row[1]) == "true",
            };

            std::string columns = value_to_string(result_row[2]);
            std::size_t offset = 0;
            while(offset <= columns.size())
            {
                const auto comma = columns.find(',', offset);
                index.columns.emplace_back(columns.substr(offset, comma == std::string::npos ? std::string::npos : comma - offset));
                if(comma == std::string::npos)
                {
                    break;
                }

                offset = comma + 1;
            }

            output.indexes.emplace_back(std::move(index));
        }

        statement foreign_keys_statement;
        foreign_keys_statement.sql =
            "SELECT kcu.column_name, ccu.table_name, ccu.column_name, rc.update_rule, rc.delete_rule "
            "FROM information_schema.table_constraints tc "
            "JOIN information_schema.key_column_usage kcu "
            "ON tc.constraint_name = kcu.constraint_name AND tc.table_schema = kcu.table_schema "
            "JOIN information_schema.constraint_column_usage ccu "
            "ON ccu.constraint_name = tc.constraint_name AND ccu.table_schema = tc.table_schema "
            "JOIN information_schema.referential_constraints rc "
            "ON rc.constraint_name = tc.constraint_name AND rc.constraint_schema = tc.table_schema "
            "WHERE tc.constraint_type = 'FOREIGN KEY' "
            "AND tc.table_schema = current_schema() AND tc.table_name = ?";
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

    // Named server side prepared statements, kept in an LRU like the SQLite
    // backend. A cache hit sends PQexecPrepared/PQsendQueryPrepared, so it
    // skips parsing and planning on the server.
    void set_statement_cache_capacity(std::size_t capacity) override
    {
        const std::lock_guard lock{mutex_};
        capacity_ = capacity;

        if(capacity_ == 0)
        {
            clear_statement_cache_locked();
            return;
        }

        while(cache_.size() > capacity_)
        {
            deallocate_locked(cache_.back().second);
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

    void set_batch_parameter_limit(std::size_t limit) override
    {
        std::lock_guard lock{mutex_};
        batch_parameter_limit_ = limit == 0 ? 1 : limit;
    }

    [[nodiscard]] std::size_t batch_parameter_limit() const noexcept override
    {
        const std::lock_guard lock{mutex_};
        return batch_parameter_limit_;
    }

    void set_logger(std::function<void(std::string_view)> logger) override
    {
        std::lock_guard lock{mutex_};
        logger_ = std::move(logger);
    }

private:
    // PREPARE accepts SELECT, INSERT, UPDATE, DELETE, VALUES and WITH; other
    // statements (DDL, SET, COPY, ...) go through the plain execution path.
    // Multi statement input is never prepared either.
    [[nodiscard]] static bool is_preparable(std::string_view sql)
    {
        const auto begin = sql.find_first_not_of(" \t\r\n");
        if(begin == std::string_view::npos)
        {
            return false;
        }

        if(const auto semicolon = sql.find(';'); semicolon != std::string_view::npos)
        {
            if(sql.substr(semicolon + 1).find_first_not_of(" \t\r\n;") != std::string_view::npos)
            {
                return false;
            }
        }

        std::string keyword;
        for(std::size_t index = begin; index < sql.size(); ++index)
        {
            const auto character = static_cast<unsigned char>(sql[index]);
            if(!std::isalpha(character))
            {
                break;
            }

            keyword.push_back(static_cast<char>(std::toupper(character)));
        }

        return keyword == "SELECT" || keyword == "INSERT" || keyword == "UPDATE" || keyword == "DELETE" ||
               keyword == "VALUES" || keyword == "WITH";
    }

    [[nodiscard]] static bool is_ddl(std::string_view sql)
    {
        const auto begin = sql.find_first_not_of(" \t\r\n");
        if(begin == std::string_view::npos)
        {
            return false;
        }

        std::string keyword;
        for(std::size_t index = begin; index < sql.size(); ++index)
        {
            const auto character = static_cast<unsigned char>(sql[index]);
            if(!std::isalpha(character))
            {
                break;
            }

            keyword.push_back(static_cast<char>(std::toupper(character)));
        }

        return keyword == "CREATE" || keyword == "ALTER" || keyword == "DROP" || keyword == "TRUNCATE" ||
               keyword == "RENAME";
    }

    // Returns the prepared statement name for the SQL, or an empty string when
    // the statement should run unprepared. Caller holds the mutex.
    [[nodiscard]] std::string statement_name_for(const std::string& sql)
    {
        if(capacity_ == 0 || !is_preparable(sql))
        {
            return {};
        }

        if(const auto found = index_.find(sql); found != index_.end())
        {
            cache_.splice(cache_.begin(), cache_, found->second);
            return cache_.front().second;
        }

        const auto name = "entityfy_stmt_" + std::to_string(next_statement_id_++);

        if(!prepare_locked(name, sql))
        {
            // Not preparable in this server: keep using the plain path instead
            // of failing the statement.
            return {};
        }

        cache_.emplace_front(sql, name);
        index_[sql] = cache_.begin();

        while(cache_.size() > capacity_)
        {
            deallocate_locked(cache_.back().second);
            index_.erase(cache_.back().first);
            cache_.pop_back();
        }

        return cache_.front().second;
    }

    // Runs a statement, preferring the prepared plan. A prepared statement can
    // disappear underneath us (PREPARE is undone when the transaction that ran
    // it rolls back), so a failed prepared call falls back to the plain path
    // and forgets the plan instead of surfacing a confusing error.
    [[nodiscard]] pg_result_ptr run_statement(const std::string& sql,
                                              const std::string& prepared,
                                              const pg_parameters& bound)
    {
        if(prepared.empty())
        {
            return execute_params(sql, bound);
        }

        try
        {
            return execute_prepared(prepared, bound);
        }
        catch(const std::exception&)
        {
            forget_statement(sql);
            return execute_params(sql, bound);
        }
    }

    [[nodiscard]] pg_result_ptr execute_params(const std::string& sql, const pg_parameters& bound)
    {
        return validate_result(
            PQexecParams(connection_.get(),
                         sql.c_str(),
                         bound.count(),
                         nullptr,
                         bound.values(),
                         bound.lengths(),
                         bound.formats(),
                         0)
        );
    }

    [[nodiscard]] pg_result_ptr execute_prepared(const std::string& name, const pg_parameters& bound)
    {
        return validate_result(
            PQexecPrepared(connection_.get(),
                           name.c_str(),
                           bound.count(),
                           bound.values(),
                           bound.lengths(),
                           bound.formats(),
                           0)
        );
    }

    // A COPY puts the connection into the copy protocol, and every later
    // statement fails until the stream is finished. The blocking path therefore
    // ends (COPY IN) or consumes (COPY OUT) the stream and takes the result
    // that concludes it, so the connection can carry the next statement.
    void discard_copy_sync(ExecStatusType status)
    {
        PGconn* const connection = connection_.get();

        if(status == PGRES_COPY_IN || status == PGRES_COPY_BOTH)
        {
            // An error message marks the copy as abandoned instead of letting
            // the server accept an empty stream as a successful copy.
            (void)PQputCopyEnd(connection, "COPY is not supported by entityfy");
        }

        if(status == PGRES_COPY_OUT || status == PGRES_COPY_BOTH)
        {
            for(;;)
            {
                char*     buffer = nullptr;
                const int chunk = PQgetCopyData(connection, &buffer, 0);

                if(chunk > 0)
                {
                    PQfreemem(buffer);
                    continue;
                }

                // -1: the server finished the copy; -2: it already failed.
                break;
            }
        }

        while(PGresult* trailing = PQgetResult(connection))
        {
            PQclear(trailing);
        }
    }

    [[nodiscard]] pg_result_ptr validate_result(PGresult* raw)
    {
        if(raw == nullptr)
        {
            throw std::runtime_error{connection_message(connection_.get())};
        }

        pg_result_ptr result{raw};
        const auto   status = PQresultStatus(raw);

        if(status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK)
        {
            return result;
        }

        if(status == PGRES_EMPTY_QUERY)
        {
            throw std::runtime_error{"unexpected empty query"};
        }

        if(is_copy_status(status))
        {
            // The statement switched the connection into the copy protocol.
            // entityfy cannot deliver copy data, so the stream is discarded and
            // the statement reported as unsupported.
            discard_copy_sync(status);
            throw pg_statement_error{"COPY is not supported by the entityfy PostgreSQL backend", "0A000"};
        }

        auto info = result_error_info(raw);
        throw pg_statement_error{std::move(info.message), std::move(info.sqlstate)};
    }

    // libpq's blocking pipeline sequence is: statement result, null (end of
    // that query's results), next statement result, ..., pipeline sync, null.
    // Only the nulls between results are skipped here.
    [[nodiscard]] PGresult* next_pipeline_result()
    {
        while(true)
        {
            if(PQconsumeInput(connection_.get()) == 0)
            {
                return nullptr;
            }

            if(PGresult* result = PQgetResult(connection_.get()); result != nullptr)
            {
                return result;
            }
        }
    }

    void flush_pipeline()
    {
        while(true)
        {
            const int state = PQflush(connection_.get());
            if(state == 0)
            {
                return;
            }

            if(state < 0)
            {
                throw std::runtime_error{"unable to flush pipeline: " + connection_message(connection_.get())};
            }
        }
    }

    // Leaves pipeline mode after a protocol level failure. A connection that
    // cannot leave pipeline mode is reset: otherwise every later call fails
    // with "not allowed in pipeline mode" instead of the real problem.
    void abandon_pipeline_mode() noexcept
    {
        if(PQexitPipelineMode(connection_.get()) != 0)
        {
            return;
        }

        PQreset(connection_.get());

        // The reset starts a fresh session, so the server side plans are gone
        // as well and the local cache must not hand their names out again.
        cache_.clear();
        index_.clear();
    }

    // Every statement is queued first and only then followed by the pipeline
    // sync, so the whole batch leaves in one round trip. A statement failure
    // aborts the rest of the pipeline; the remaining results are drained
    // through the sync before the error is rethrown, which keeps the
    // connection usable.
    [[nodiscard]] std::vector<execution_result> run_pipeline(
        const std::vector<entityfy::statement>& statements,
        const std::vector<std::string>& sqls,
        const std::vector<std::string>& prepared_names)
    {
        if(PQenterPipelineMode(connection_.get()) == 0)
        {
            throw std::runtime_error{"unable to enter pipeline mode: " + connection_message(connection_.get())};
        }

        bool in_pipeline = true;
        // True from the moment the sync is sent until its result and the
        // terminator have been consumed: only then can the connection be
        // drained and be used again.
        bool results_pending = false;

        std::vector<execution_result> output;
        output.reserve(statements.size());

        pg_error_info failure;
        bool          failed = false;

        try
        {
            for(std::size_t index = 0; index < statements.size(); ++index)
            {
                if(logger_)
                {
                    logger_(statements[index].sql);
                }

                auto parameters = to_pg_parameters(statements[index].binds);
                const pg_parameters bound{parameters};

                const std::string& prepared = prepared_names[index];
                const auto         cached = index_.find(sqls[index]);
                const bool         usable =
                    !prepared.empty() && cached != index_.end() && cached->second->second == prepared;

                const int sent = usable
                    ? PQsendQueryPrepared(connection_.get(),
                                          prepared.c_str(),
                                          bound.count(),
                                          bound.values(),
                                          bound.lengths(),
                                          bound.formats(),
                                          0)
                    : PQsendQueryParams(connection_.get(),
                                        sqls[index].c_str(),
                                        bound.count(),
                                        nullptr,
                                        bound.values(),
                                        bound.lengths(),
                                        bound.formats(),
                                        0);

                if(sent == 0)
                {
                    throw std::runtime_error{"unable to send pipeline statement: " + connection_message(connection_.get())};
                }
            }

            if(PQpipelineSync(connection_.get()) == 0)
            {
                throw std::runtime_error{"unable to sync pipeline: " + connection_message(connection_.get())};
            }

            results_pending = true;
            flush_pipeline();

            for(std::size_t index = 0; index < statements.size(); ++index)
            {
                pg_result_ptr result{next_pipeline_result()};

                if(result == nullptr)
                {
                    throw std::runtime_error{"unable to obtain pipeline result: " + connection_message(connection_.get())};
                }

                if(failed)
                {
                    continue;
                }

                const auto status = PQresultStatus(result.get());
                if(status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK)
                {
                    output.push_back(execution_result{.rows_affected = rows_affected(result.get())});
                }
                else
                {
                    failed = true;
                    failure = result_error_info(result.get());
                }
            }

            {
                pg_result_ptr sync{next_pipeline_result()};
                if(sync == nullptr)
                {
                    throw std::runtime_error{"pipeline sync result was lost: " + connection_message(connection_.get())};
                }

                if(!failed && PQresultStatus(sync.get()) != PGRES_PIPELINE_SYNC)
                {
                    failed = true;
                    failure = result_error_info(sync.get());
                }
            }

            // The pipeline terminates with a null result after the sync.
            pg_result_ptr trailing{PQgetResult(connection_.get())};
            (void)trailing;
            results_pending = false;

            if(PQexitPipelineMode(connection_.get()) == 0)
            {
                throw std::runtime_error{"unable to leave pipeline mode: " + connection_message(connection_.get())};
            }

            in_pipeline = false;
        }
        catch(...)
        {
            if(results_pending)
            {
                // Consume the remaining results through the sync so the
                // connection can leave pipeline mode and be used again.
                for(std::size_t remaining = statements.size() + 1; remaining > 0; --remaining)
                {
                    pg_result_ptr result{next_pipeline_result()};
                    if(result == nullptr)
                    {
                        break;
                    }

                    if(PQresultStatus(result.get()) == PGRES_PIPELINE_SYNC)
                    {
                        break;
                    }
                }

                // Consume the pipeline terminator before leaving pipeline mode.
                pg_result_ptr trailing{PQgetResult(connection_.get())};
                (void)trailing;
            }

            if(in_pipeline)
            {
                abandon_pipeline_mode();
            }

            throw;
        }

        if(failed)
        {
            throw pg_statement_error{failure.message, failure.sqlstate};
        }

        return output;
    }

    [[nodiscard]] bool prepare_locked(const std::string& name, const std::string& sql)
    {
        pg_result_ptr result{PQprepare(connection_.get(), name.c_str(), sql.c_str(), 0, nullptr)};
        return result != nullptr && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
    }

    void forget_statement(const std::string& sql)
    {
        const auto found = index_.find(sql);
        if(found == index_.end())
        {
            return;
        }

        deallocate_locked(found->second->second);
        cache_.erase(found->second);
        index_.erase(found);
    }

    // Drops a cached plan without asking the server. Called when the server
    // already reported the name as unknown (SQLSTATE 26000), so a DEALLOCATE
    // would only fail again — and on a nonblocking connection it would need a
    // command stream of its own.
    void forget_statement_local(const std::string& sql) noexcept
    {
        const auto found = index_.find(sql);
        if(found == index_.end())
        {
            return;
        }

        cache_.erase(found->second);
        index_.erase(found);
    }

    void deallocate_locked(const std::string& name) noexcept
    {
        try
        {
            pg_result_ptr result{PQexec(connection_.get(), ("DEALLOCATE \"" + name + "\"").c_str())};
            (void)result;
        }
        catch(const std::exception&)
        {
        }
    }

    void clear_statement_cache_locked() noexcept
    {
        for(const auto& entry: cache_)
        {
            deallocate_locked(entry.second);
        }

        cache_.clear();
        index_.clear();
    }

    // ---------------------------------------------------------------------
    // Native asynchronous execution
    // ---------------------------------------------------------------------

    // A synchronous caller never shares the connection with the reactor: it
    // throws instead of blocking on a connection that is mid-statement.
    void require_idle_connection() const
    {
        if(busy_.load(std::memory_order_acquire))
        {
            throw std::logic_error{"connection busy with an async operation"};
        }
    }

    void start_reactor()
    {
        const std::lock_guard lock{queue_mutex_};

        if(!reactor_.joinable())
        {
            reactor_ = std::thread{[this] { reactor_loop(); }};
        }
    }

    void enqueue_job(std::function<void()> job)
    {
        {
            const std::lock_guard lock{queue_mutex_};
            queue_.push_back(std::move(job));
        }

        queue_cv_.notify_one();
    }

    // Drains the queue and exits once the backend stops. A job that was queued
    // before the stop still runs, so its task completes instead of hanging.
    void reactor_loop()
    {
        {
            const std::lock_guard lock{queue_mutex_};
            reactor_id_ = std::this_thread::get_id();
        }

        for(;;)
        {
            std::function<void()> job;
            {
                std::unique_lock lock{queue_mutex_};
                queue_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });

                if(queue_.empty())
                {
                    return;
                }

                job = std::move(queue_.front());
                queue_.pop_front();
            }

            job();
        }
    }

    // Claims the single-flight slot, hands the completion to the reactor and
    // returns the observing handle. The cancel hook only touches the shared
    // flag, never the backend, so a cancel racing the destruction of the
    // backend stays valid.
    template <typename T, typename Work>
    [[nodiscard]] entityfy::task<T> submit(Work work)
    {
        auto pair = entityfy::make_task<T>();
        auto completer = std::make_shared<entityfy::task_completer<T>>(std::move(pair.completer));
        auto cancel = std::make_shared<std::atomic<bool>>(false);

        // Starting the thread first keeps a thread creation failure synchronous
        // instead of producing a task that never completes.
        start_reactor();

        bool expected = false;
        if(!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            throw std::logic_error{
                "entityfy PostgreSQL backend runs one async operation at a time: another async operation is in flight"
            };
        }

        completer->set_cancel_hook([cancel] { cancel->store(true, std::memory_order_release); });

        try
        {
            enqueue_job([this, work = std::move(work), completer, cancel]() mutable {
                run_async_job<T>(std::move(work), *completer, cancel);
            });
        }
        catch(...)
        {
            busy_.store(false, std::memory_order_release);
            completer->set_exception(std::current_exception());
        }

        return std::move(pair.value);
    }

    // One job body. The connection is held exclusively for the whole nonblocking
    // exchange, and the flight flag is cleared before the mutex is released so a
    // synchronous call that finds the connection free also finds the flag clear.
    template <typename T, typename Work>
    void run_async_job(Work work, entityfy::task_completer<T>& completer,
                       const std::shared_ptr<std::atomic<bool>>& cancel) noexcept
    {
        std::optional<T> value;
        std::exception_ptr error;
        bool cancelled = false;

        {
            const std::unique_lock lock{mutex_};
            flight_release release{busy_};

            try
            {
                // A cancel that arrived before the reactor picked the job up
                // must not send anything at all.
                if(cancel->load(std::memory_order_acquire))
                {
                    cancelled = true;
                }
                else
                {
                    value.emplace(work(cancel));
                }
            }
            catch(const async_cancelled&)
            {
                cancelled = true;
            }
            catch(...)
            {
                error = std::current_exception();
            }
        }

        // Completion runs unlocked, so a continuation may use the connection.
        if(cancelled)
        {
            completer.set_cancelled();
            return;
        }

        if(error)
        {
            completer.set_exception(std::move(error));
            return;
        }

        completer.set_value(std::move(*value));
    }

    // Polls the connection socket once. A timeout is not an error: the caller
    // loops, which is how cancellation and shutdown are observed.
    void wait_for_socket(int for_read, int for_write)
    {
        const int socket = PQsocket(connection_.get());
        if(socket < 0)
        {
            throw std::runtime_error{"entityfy PostgreSQL async connection has no socket"};
        }

        const pg_usec_time_t deadline = PQgetCurrentTimeUSec() + async_poll_interval_us;

        if(PQsocketPoll(socket, for_read, for_write, deadline) < 0)
        {
            throw std::runtime_error{
                "entityfy PostgreSQL async socket poll failed: " + connection_message(connection_.get())
            };
        }
    }

    void flush_async()
    {
        for(;;)
        {
            const int state = PQflush(connection_.get());

            if(state == 0)
            {
                return;
            }

            if(state < 0)
            {
                throw std::runtime_error{"unable to flush the async statement: " + connection_message(connection_.get())};
            }

            wait_for_socket(0, 1);
        }
    }

    // Delivers one cancel request from a separate connection, which is what lets
    // the reactor keep draining the original one. Returns true only when the
    // request reached the server; it is abandoned when it cannot be delivered
    // within the cancel budget, in which case the statement keeps running.
    [[nodiscard]] bool request_cancel()
    {
        struct cancel_deleter
        {
            void operator()(PGcancelConn* cancellation) const noexcept
            {
                if(cancellation != nullptr)
                {
                    PQcancelFinish(cancellation);
                }
            }
        };

        const std::unique_ptr<PGcancelConn, cancel_deleter> cancellation{PQcancelCreate(connection_.get())};

        if(cancellation == nullptr)
        {
            return false;
        }

        if(PQcancelStart(cancellation.get()) == 0)
        {
            return false;
        }

        const pg_usec_time_t give_up = PQgetCurrentTimeUSec() + async_cancel_budget_us;

        for(;;)
        {
            const auto status = PQcancelPoll(cancellation.get());

            if(status == PGRES_POLLING_OK)
            {
                return true;
            }

            if(status == PGRES_POLLING_FAILED)
            {
                return false;
            }

            if(status != PGRES_POLLING_READING && status != PGRES_POLLING_WRITING)
            {
                return false;
            }

            const pg_usec_time_t now = PQgetCurrentTimeUSec();
            if(now >= give_up)
            {
                return false;
            }

            const int socket = PQcancelSocket(cancellation.get());
            if(socket < 0)
            {
                return false;
            }

            const int ready = PQsocketPoll(
                socket,
                status == PGRES_POLLING_READING ? 1 : 0,
                status == PGRES_POLLING_WRITING ? 1 : 0,
                now + async_poll_interval_us
            );

            if(ready < 0)
            {
                return false;
            }
        }
    }

    // Observes the shared cancel flag and delivers at most one cancel request
    // for the operation. A request that cannot be delivered is not retried: the
    // statement simply runs to completion.
    void observe_cancel(const std::shared_ptr<std::atomic<bool>>& cancel, cancel_state& cancellation)
    {
        if(cancellation.requested || !cancel->load(std::memory_order_acquire))
        {
            return;
        }

        cancellation.requested = true;
        cancellation.delivered = request_cancel();
    }

    // Waits for one complete result, or for the terminating null, and raises the
    // cancel request once the shared flag is observed.
    [[nodiscard]] pg_result_ptr take_result_async(const std::shared_ptr<std::atomic<bool>>& cancel,
                                                  cancel_state& cancellation)
    {
        for(;;)
        {
            observe_cancel(cancel, cancellation);

            if(PQisBusy(connection_.get()) == 0)
            {
                return pg_result_ptr{PQgetResult(connection_.get())};
            }

            wait_for_socket(1, 0);

            if(PQconsumeInput(connection_.get()) == 0)
            {
                throw std::runtime_error{
                    "unable to read the async result: " + connection_message(connection_.get())
                };
            }
        }
    }

    // Reads the next real result of a protocol pipeline and skips the null
    // libpq reports at the end of every command's result set. The next
    // command's result is normally already buffered, so libpq is asked again
    // before the socket is polled: polling first would add a whole poll
    // interval to every statement of the batch. The socket is only awaited
    // while PQisBusy() reports that no complete result has been decoded yet.
    [[nodiscard]] pg_result_ptr next_pipeline_result_async(const std::shared_ptr<std::atomic<bool>>& cancel,
                                                           cancel_state& cancellation)
    {
        // One null may separate the previous command's results from the next
        // result; a second null without any new input means the stream ended.
        for(std::size_t separators = 0;;)
        {
            observe_cancel(cancel, cancellation);

            if(PQisBusy(connection_.get()) == 0)
            {
                if(PGresult* raw = PQgetResult(connection_.get()); raw != nullptr)
                {
                    if(is_copy_status(PQresultStatus(raw)))
                    {
                        // A pipeline cannot deliver copy data either, and it
                        // cannot be drained in place: report it so the caller
                        // abandons pipeline mode instead of looping.
                        PQclear(raw);
                        throw std::runtime_error{"COPY is not supported inside a PostgreSQL pipeline"};
                    }

                    return pg_result_ptr{raw};
                }

                if(++separators > 1)
                {
                    return {};
                }

                continue;
            }

            wait_for_socket(1, 0);

            if(PQconsumeInput(connection_.get()) == 0)
            {
                throw std::runtime_error{
                    "unable to read the async pipeline result: " + connection_message(connection_.get())
                };
            }
        }
    }

    // Ends the COPY the server started and consumes the stream, so the
    // connection leaves the copy protocol and the final result that concludes
    // the copy becomes readable. entityfy has no copy API, so the data is
    // discarded instead of delivered.
    void discard_copy_async(ExecStatusType status)
    {
        PGconn* const connection = connection_.get();

        if(status == PGRES_COPY_IN || status == PGRES_COPY_BOTH)
        {
            // The error message marks the copy as abandoned; otherwise the
            // server would accept an empty stream as a successful copy. A
            // COPY done in extended query mode is followed by a Sync, which is
            // what lets the connection continue with normal statements.
            (void)PQputCopyEnd(connection, "COPY is not supported by entityfy");
            flush_async();
        }

        if(status == PGRES_COPY_OUT || status == PGRES_COPY_BOTH)
        {
            for(;;)
            {
                char*     buffer = nullptr;
                const int chunk = PQgetCopyData(connection, &buffer, 1);

                if(chunk > 0)
                {
                    PQfreemem(buffer);
                    continue;
                }

                if(chunk != 0)
                {
                    // -1: the copy ended; -2: the protocol already failed and
                    // the drain below still has to consume what arrived.
                    break;
                }

                // No complete chunk buffered yet: load more from the socket.
                wait_for_socket(1, 0);

                if(PQconsumeInput(connection) == 0)
                {
                    throw std::runtime_error{
                        "unable to discard the async COPY stream: " + connection_message(connection)
                    };
                }
            }
        }
    }

    // Every result up to the terminating null has to leave the input buffer,
    // otherwise the next statement on this connection would read stale results.
    // A COPY result is consumed as well: libpq keeps answering every
    // PQgetResult in copy state with a fresh COPY result instead of the
    // terminating null, so an unguarded drain would never finish. The bound
    // keeps a protocol surprise from spinning forever.
    void drain_results_async(const std::shared_ptr<std::atomic<bool>>& cancel, cancel_state& cancellation)
    {
        for(std::size_t guard = 0; guard < 4; ++guard)
        {
            pg_result_ptr result{take_result_async(cancel, cancellation)};

            if(result == nullptr)
            {
                return;
            }

            if(const auto status = PQresultStatus(result.get()); is_copy_status(status))
            {
                discard_copy_async(status);
            }
        }

        throw std::runtime_error{"the async PostgreSQL result stream did not terminate"};
    }

    // A server side abort caused by our own cancel request completes the task as
    // cancelled; every other failure keeps the regular classification, because
    // the server may have aborted on its own (statement_timeout, ...).
    [[noreturn]] void deliver_async_failure(const pg_error_info& failure, const cancel_state& cancellation)
    {
        if(failure.sqlstate == "57014" && cancellation.delivered)
        {
            throw async_cancelled{};
        }

        throw pg_statement_error{failure.message, failure.sqlstate};
    }

    // Sends one statement and returns its result, or throws after the result
    // stream has been drained. A prepared plan that the server no longer knows
    // (SQLSTATE 26000) is retried once, plain: the name is rejected before
    // anything executes, so the retry cannot run the statement twice.
    [[nodiscard]] pg_result_ptr send_and_collect_single(const std::string& sql,
                                                        const std::string& prepared,
                                                        const pg_parameters& bound,
                                                        const std::shared_ptr<std::atomic<bool>>& cancel)
    {
        if(prepared.empty())
        {
            return send_and_collect_once(sql, {}, bound, cancel);
        }

        try
        {
            return send_and_collect_once(sql, prepared, bound, cancel);
        }
        catch(const pg_statement_error& error)
        {
            if(error.sqlstate() != sqlstate_invalid_statement_name)
            {
                throw;
            }

            // The plan disappeared underneath the connection, so it is dropped
            // locally and the statement re-sent unprepared.
            forget_statement_local(sql);
            return send_and_collect_once(sql, {}, bound, cancel);
        }
    }

    [[nodiscard]] pg_result_ptr send_and_collect_once(const std::string& sql,
                                                      const std::string& prepared,
                                                      const pg_parameters& bound,
                                                      const std::shared_ptr<std::atomic<bool>>& cancel)
    {
        // A cancel that was requested before the statement reached the socket
        // must not send it at all.
        if(cancel->load(std::memory_order_acquire))
        {
            throw async_cancelled{};
        }

        const int sent = prepared.empty()
            ? PQsendQueryParams(connection_.get(),
                                sql.c_str(),
                                bound.count(),
                                nullptr,
                                bound.values(),
                                bound.lengths(),
                                bound.formats(),
                                0)
            : PQsendQueryPrepared(connection_.get(),
                                  prepared.c_str(),
                                  bound.count(),
                                  bound.values(),
                                  bound.lengths(),
                                  bound.formats(),
                                  0);

        if(sent == 0)
        {
            throw std::runtime_error{"unable to send the async statement: " + connection_message(connection_.get())};
        }

        flush_async();

        cancel_state cancellation;

        pg_result_ptr result{take_result_async(cancel, cancellation)};
        if(result == nullptr)
        {
            throw std::runtime_error{
                "the async statement produced no result: " + connection_message(connection_.get())
            };
        }

        pg_error_info failure;
        bool failed = false;

        const auto status = PQresultStatus(result.get());

        if(is_copy_status(status))
        {
            // The server switched the connection into the copy protocol. The
            // stream is abandoned so the connection stays usable, and the
            // statement is reported as unsupported: entityfy cannot deliver
            // copy data. Without this the drain would keep receiving fresh
            // COPY results and never see its terminating null.
            discard_copy_async(status);
            drain_results_async(cancel, cancellation);

            throw pg_statement_error{"COPY is not supported by the entityfy PostgreSQL backend", "0A000"};
        }

        if(status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
        {
            failed = true;
            failure = result_error_info(result.get());
        }

        drain_results_async(cancel, cancellation);

        if(failed)
        {
            deliver_async_failure(failure, cancellation);
        }

        return result;
    }

    // The unclassified form: a caller that batches several statements classifies
    // the failure once, against the batch.
    execution_result run_execute_async(const entityfy::statement& input,
                                       const std::shared_ptr<std::atomic<bool>>& cancel)
    {
        if(logger_)
        {
            logger_(input.sql);
        }

        auto sql = postgres_placeholders(input.sql);
        auto parameters = to_pg_parameters(input.binds);
        const pg_parameters bound{parameters};

        const bool ddl = is_ddl(sql);
        const std::string prepared = statement_name_for(sql);

        pg_result_ptr result;
        {
            const nonblocking_scope nonblocking{connection_.get()};
            result = send_and_collect_single(sql, prepared, bound, cancel);
        }

        // A prepared plan captures the schema at prepare time; after DDL the
        // cached plans may refer to a dropped or altered relation.
        if(ddl)
        {
            clear_statement_cache_locked();
        }

        return execution_result{.rows_affected = rows_affected(result.get())};
    }

    execution_result execute_async_locked(const entityfy::statement& input,
                                          const std::shared_ptr<std::atomic<bool>>& cancel)
    {
        try
        {
            return run_execute_async(input, cancel);
        }
        catch(const async_cancelled&)
        {
            throw;
        }
        catch(const pg_statement_error& error)
        {
            throw_pg_error("execute", input.sql, error.sqlstate(), error.what());
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("PostgreSQL", "execute", input.sql, error.what());
        }
    }

    [[nodiscard]] std::vector<row> query_async_locked(const entityfy::statement& input,
                                                      const std::shared_ptr<std::atomic<bool>>& cancel)
    {
        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            auto sql = postgres_placeholders(input.sql);
            auto parameters = to_pg_parameters(input.binds);
            const pg_parameters bound{parameters};

            const std::string prepared = statement_name_for(sql);

            pg_result_ptr result;
            {
                const nonblocking_scope nonblocking{connection_.get()};
                result = send_and_collect_single(sql, prepared, bound, cancel);
            }

            std::vector<row> rows;
            const int count = PQntuples(result.get());
            rows.reserve(static_cast<std::size_t>(count));

            for(int index = 0; index < count; ++index)
            {
                row output;
                append_row(result.get(), index, output);
                rows.emplace_back(std::move(output));
            }

            return rows;
        }
        catch(const async_cancelled&)
        {
            throw;
        }
        catch(const pg_statement_error& error)
        {
            throw_pg_error("query", input.sql, error.sqlstate(), error.what());
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("PostgreSQL", "query", input.sql, error.what());
        }
    }

    [[nodiscard]] std::vector<execution_result> execute_batch_async_locked(
        const std::vector<entityfy::statement>& statements,
        const std::shared_ptr<std::atomic<bool>>& cancel)
    {
        if(statements.empty())
        {
            return {};
        }

        try
        {
            bool pipeline_ready = statements.size() > 1;

            for(const auto& entry: statements)
            {
                const auto sql = postgres_placeholders(entry.sql);

                if(is_ddl(sql) || !is_preparable(sql))
                {
                    pipeline_ready = false;
                    break;
                }
            }

            if(!pipeline_ready)
            {
                // Nothing to pipeline: the statements still travel through the
                // nonblocking path, one at a time.
                std::vector<execution_result> output;
                output.reserve(statements.size());

                for(const auto& entry: statements)
                {
                    output.push_back(run_execute_async(entry, cancel));
                }

                return output;
            }

            std::vector<std::string> sqls;
            sqls.reserve(statements.size());

            for(const auto& entry: statements)
            {
                sqls.push_back(postgres_placeholders(entry.sql));
            }

            // A pipeline batch is never retried as a whole, so a stale prepared
            // plan could not fall back to the plain path: every statement is
            // sent plain, which costs nothing extra inside one round trip.
            const nonblocking_scope nonblocking{connection_.get()};
            return run_pipeline_async(statements, sqls, cancel);
        }
        catch(const async_cancelled&)
        {
            throw;
        }
        catch(const pg_statement_error& error)
        {
            throw_pg_error("execute batch", statements.front().sql, error.sqlstate(), error.what());
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("PostgreSQL", "execute batch", statements.front().sql, error.what());
        }
    }

    // Consumes results until the pipeline sync has been seen and then the
    // pipeline terminator, leaving the connection ready for
    // PQexitPipelineMode. `remaining` bounds the drain: a pipeline can only
    // produce one result per queued statement plus the sync.
    void drain_to_pipeline_sync(const std::shared_ptr<std::atomic<bool>>& cancel, cancel_state& cancellation,
                                std::size_t remaining)
    {
        for(std::size_t guard = 0; guard < remaining + 2; ++guard)
        {
            pg_result_ptr result{next_pipeline_result_async(cancel, cancellation)};

            if(result == nullptr)
            {
                return;
            }

            if(PQresultStatus(result.get()) == PGRES_PIPELINE_SYNC)
            {
                // The pipeline terminates with a null result after the sync.
                (void)take_result_async(cancel, cancellation);
                return;
            }
        }

        throw std::runtime_error{"the async PostgreSQL pipeline did not reach its sync point"};
    }

    [[nodiscard]] std::vector<execution_result> run_pipeline_async(
        const std::vector<entityfy::statement>& statements,
        const std::vector<std::string>& sqls,
        const std::shared_ptr<std::atomic<bool>>& cancel)
    {
        // A cancel observed before anything was queued never reaches the server.
        if(cancel->load(std::memory_order_acquire))
        {
            throw async_cancelled{};
        }

        if(PQenterPipelineMode(connection_.get()) == 0)
        {
            throw std::runtime_error{"unable to enter pipeline mode: " + connection_message(connection_.get())};
        }

        cancel_state cancellation;
        bool         in_pipeline = true;
        bool         results_pending = false;
        // Statements actually queued: a cancel stops the send loop before the
        // next statement, so only this many results arrive.
        std::size_t  sent = 0;
        bool         cancelled_early = false;

        std::vector<execution_result> output;
        output.reserve(statements.size());

        pg_error_info failure;
        bool          failed = false;

        try
        {
            for(std::size_t index = 0; index < statements.size(); ++index)
            {
                // Cancellation is observed between statements: the batch stops
                // queueing as soon as the flag is up.
                if(cancel->load(std::memory_order_acquire))
                {
                    cancelled_early = true;
                    break;
                }

                if(logger_)
                {
                    logger_(statements[index].sql);
                }

                auto parameters = to_pg_parameters(statements[index].binds);
                const pg_parameters bound{parameters};

                // A batch is never retried as a whole, so a stale prepared plan
                // could not fall back to the plain path: every statement is
                // sent plain, which costs nothing extra inside one round trip.
                if(PQsendQueryParams(connection_.get(),
                                     sqls[index].c_str(),
                                     bound.count(),
                                     nullptr,
                                     bound.values(),
                                     bound.lengths(),
                                     bound.formats(),
                                     0) == 0)
                {
                    throw std::runtime_error{
                        "unable to send the async pipeline statement: " + connection_message(connection_.get())
                    };
                }

                ++sent;
            }

            if(PQpipelineSync(connection_.get()) == 0)
            {
                throw std::runtime_error{"unable to sync the async pipeline: " + connection_message(connection_.get())};
            }

            results_pending = true;
            flush_async();

            for(std::size_t index = 0; index < sent; ++index)
            {
                pg_result_ptr result{next_pipeline_result_async(cancel, cancellation)};

                if(result == nullptr)
                {
                    throw std::runtime_error{
                        "the async pipeline stopped before returning every result: " +
                        connection_message(connection_.get())
                    };
                }

                if(failed)
                {
                    continue;
                }

                const auto status = PQresultStatus(result.get());
                if(status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK)
                {
                    output.push_back(execution_result{.rows_affected = rows_affected(result.get())});
                }
                else
                {
                    failed = true;
                    failure = result_error_info(result.get());
                }
            }

            {
                pg_result_ptr sync{next_pipeline_result_async(cancel, cancellation)};

                if(sync == nullptr)
                {
                    throw std::runtime_error{
                        "the async pipeline sync result was lost: " + connection_message(connection_.get())
                    };
                }

                if(!failed && PQresultStatus(sync.get()) != PGRES_PIPELINE_SYNC)
                {
                    failed = true;
                    failure = result_error_info(sync.get());
                }
            }

            // The pipeline terminates with a null result after the sync.
            (void)take_result_async(cancel, cancellation);
            results_pending = false;

            if(PQexitPipelineMode(connection_.get()) == 0)
            {
                throw std::runtime_error{"unable to leave pipeline mode: " + connection_message(connection_.get())};
            }

            in_pipeline = false;
        }
        catch(...)
        {
            if(results_pending)
            {
                // Consume whatever is left through the sync so the connection
                // can leave pipeline mode and be reused.
                try
                {
                    drain_to_pipeline_sync(cancel, cancellation, sent + 1);
                }
                catch(const std::exception&)
                {
                }
            }

            // A failure before the sync leaves the connection in pipeline mode;
            // abandoning it (resetting when even that fails) keeps later calls
            // from failing with "not allowed in pipeline mode".
            if(in_pipeline)
            {
                abandon_pipeline_mode();
            }

            throw;
        }

        if(cancelled_early)
        {
            // What was already queued has completed; the rest was never sent.
            throw async_cancelled{};
        }

        if(failed)
        {
            deliver_async_failure(failure, cancellation);
        }

        return output;
    }

    pg_connection_ptr connection_;
    mutable std::recursive_mutex           mutex_;
    std::size_t                            batch_parameter_limit_ = max_pg_parameters;
    std::size_t                            capacity_ = 32;
    std::uint64_t                          next_statement_id_ = 1;
    std::list<std::pair<std::string, std::string>> cache_;
    std::unordered_map<std::string, decltype(cache_.begin())> index_;
    std::function<void(std::string_view)>  logger_;

    // Async reactor state. `busy_` is the single-flight guard: exactly one async
    // operation owns the connection, and the flag is only cleared while the
    // reactor still holds `mutex_`.
    std::atomic<bool>                 busy_{false};
    std::thread                       reactor_;
    // Set by the reactor itself once it starts, so the destructor can tell that
    // it runs on the thread it would have to join.
    std::thread::id                   reactor_id_;
    std::mutex                        queue_mutex_;
    std::condition_variable           queue_cv_;
    std::deque<std::function<void()>> queue_;
    bool                              stopping_ = false;
};

std::unique_ptr<backend> make_postgresql_backend(std::string connection_info)
{
    return std::make_unique<postgresql_backend>(std::move(connection_info));
}

} // namespace entityfy::detail
