#pragma once

#include <entityfy/dialect.hpp>
#include <entityfy/introspection.hpp>
#include <entityfy/statement.hpp>
#include <entityfy/value.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace entityfy::detail
{

using row = std::vector<sql_value>;

struct execution_result
{
    std::uint64_t rows_affected = 0;
    std::int64_t  last_insert_id = 0;
};

class backend
{
public:
    virtual ~backend() = default;

    [[nodiscard]] virtual dialect target_dialect() const noexcept = 0;
    virtual execution_result execute(const entityfy::statement& input) = 0;

    // Runs several independent statements in order. The default executes them
    // one by one; PostgreSQL overrides it with a protocol pipeline so the
    // statements travel in one round trip (prepared statements are cached as
    // they are for single statements).
    [[nodiscard]] virtual std::vector<execution_result> execute_batch(
        const std::vector<entityfy::statement>& statements)
    {
        std::vector<execution_result> output;
        output.reserve(statements.size());

        for(const auto& current: statements)
        {
            output.push_back(execute(current));
        }

        return output;
    }
    [[nodiscard]] virtual std::vector<row> query(const entityfy::statement& input) = 0;

    // Streams rows to a consumer without collecting them: the backend reuses a
    // single row buffer, so the consumer has to materialize (copy out) what it
    // needs inside the callback. Returning false stops the iteration, which is
    // how single-row reads avoid fetching the rest.
    virtual void query_each(const entityfy::statement& input,
                            const std::function<bool(const row&)>& consumer)
    {
        for(const auto& current: query(input))
        {
            if(!consumer(current))
            {
                return;
            }
        }
    }
    [[nodiscard]] virtual std::vector<std::string> table_columns(std::string_view table) = 0;
    [[nodiscard]] virtual entityfy::table_info inspect_table(std::string_view table) = 0;

    // Prepared statement cache. Backends that cannot reuse compiled statements
    // report a capacity of zero and ignore the configuration.
    virtual void set_statement_cache_capacity(std::size_t capacity)
    {
        (void)capacity;
    }

    [[nodiscard]] virtual std::size_t statement_cache_capacity() const noexcept
    {
        return 0;
    }

    [[nodiscard]] virtual std::size_t statement_cache_size() const noexcept
    {
        return 0;
    }

    virtual void clear_statement_cache()
    {
    }

    // Maximum number of bind parameters a single statement may use. Batch
    // writers split their work by this limit.
    virtual void set_batch_parameter_limit(std::size_t limit)
    {
        (void)limit;
    }

    [[nodiscard]] virtual std::size_t batch_parameter_limit() const noexcept
    {
        return 0;
    }

    // Optional SQL logger; receives every statement before it is executed.
    virtual void set_logger(std::function<void(std::string_view)> logger)
    {
        (void)logger;
    }
};

std::unique_ptr<backend> make_sqlite_backend(std::string path);
std::unique_ptr<backend> make_postgresql_backend(std::string connection_info);
std::unique_ptr<backend> make_mysql_backend(std::string connection_info);
std::unique_ptr<backend> make_sqlserver_backend(std::string uri);

[[nodiscard]] std::string postgres_placeholders(std::string_view sql);

} // namespace entityfy::detail
