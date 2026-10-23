#include <entityfy/detail/backend.hpp>
#include <entityfy/error.hpp>
#include <entityfy/executor.hpp>
#include <entityfy/statement.hpp>

#include <sl3.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <limits>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace entityfy::detail
{
namespace
{

[[nodiscard]] sl3::DbValue to_sl3_value(const sql_value& value)
{
    return std::visit(
        [](const auto& current) -> sl3::DbValue {
            using current_type = std::remove_cvref_t<decltype(current)>;

            if constexpr(std::same_as<current_type, std::monostate>)
            {
                return sl3::DbValue{sl3::Type::Null};
            }
            else if constexpr(std::same_as<current_type, bool>)
            {
                return sl3::DbValue{static_cast<std::int64_t>(current ? 1 : 0)};
            }
            else if constexpr(std::same_as<current_type, std::int64_t>)
            {
                return sl3::DbValue{current};
            }
            else if constexpr(std::same_as<current_type, std::uint64_t>)
            {
                if(current > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                {
                    throw std::overflow_error{"SQLite INTEGER cannot store this uint64_t value"};
                }

                return sl3::DbValue{static_cast<std::int64_t>(current)};
            }
            else if constexpr(std::same_as<current_type, double>)
            {
                return sl3::DbValue{current};
            }
            else if constexpr(std::same_as<current_type, std::string>)
            {
                return sl3::DbValue{current};
            }
            else
            {
                return sl3::DbValue{sl3::Blob{current.begin(), current.end()}};
            }
        },
        value
    );
}

[[nodiscard]] sl3::DbValues to_sl3_values(const std::vector<sql_value>& values)
{
    std::vector<sl3::DbValue> converted;
    converted.reserve(values.size());

    for(const auto& value: values)
    {
        converted.emplace_back(to_sl3_value(value));
    }

    return sl3::DbValues{std::move(converted)};
}

[[nodiscard]] sql_value from_sl3_column(const sl3::Columns& columns, int index)
{
    switch(columns.getType(index))
    {
        case sl3::Type::Null:
            return std::monostate{};

        case sl3::Type::Int:
            return static_cast<std::int64_t>(columns.getInt64(index));

        case sl3::Type::Real:
            return columns.getReal(index);

        case sl3::Type::Text:
            return columns.getText(index);

        case sl3::Type::Blob:
            return bytes{columns.getBlob(index)};

        case sl3::Type::Variant:
            break;
    }

    return columns.getText(index);
}

// sl3 only exposes the sqlite3 message text, so busy/locked are recognized by
// the message and get the typed error a retry can look at.
[[noreturn]] void throw_sqlite_error(std::string_view operation,
                                     const std::string& sql,
                                     const std::exception& error)
{
    const std::string_view message{error.what()};

    if(message.find("database is locked") != std::string_view::npos ||
       message.find("database table is locked") != std::string_view::npos ||
       message.find("database schema is locked") != std::string_view::npos)
    {
        throw database_busy{sql_context_message("SQLite", operation, sql, error.what()), sql};
    }

    throw_classified_sql_error("SQLite", operation, sql, error.what());
}

} // namespace

class sqlite_backend final : public backend
{
public:
    explicit sqlite_backend(std::string path)
        : database_{std::move(path)}
    {
        execute(statement{.sql = "PRAGMA foreign_keys = ON"});
        execute(statement{.sql = "PRAGMA busy_timeout = 5000"});
    }

    [[nodiscard]] dialect target_dialect() const noexcept override
    {
        return dialect::sqlite;
    }

    // ---------------------------------------------------------------------
    // Asynchronous execution
    //
    // One SQLite connection is a serialized resource, so the async surface
    // offloads the existing synchronous entry points to one bounded worker.
    // The worker queue keeps submission order and the connection mutex
    // serializes execution, but ordering is per statement, not per batch: the
    // mutex is taken and released around each statement, so a synchronous call
    // can interleave between the statements of an async batch. Cancellation is
    // cooperative: it prevents a queued job from starting, but a statement
    // already inside SQLite cannot be interrupted.
    // ---------------------------------------------------------------------
    [[nodiscard]] async_capability async_mode() const noexcept override
    {
        return async_capability::thread_offload;
    }

    entityfy::task<execution_result> execute_async(const entityfy::statement& input) override
    {
        return offload<execution_result>([this, input] { return execute(input); });
    }

    entityfy::task<std::vector<row>> query_async(const entityfy::statement& input) override
    {
        return offload<std::vector<row>>([this, input] { return query(input); });
    }

    entityfy::task<std::vector<execution_result>> execute_batch_async(
        const std::vector<entityfy::statement>& statements) override
    {
        return offload<std::vector<execution_result>>([this, statements] { return execute_batch(statements); });
    }

    execution_result execute(const entityfy::statement& input) override
    {
        std::lock_guard lock{mutex_};

        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            auto& command = command_for(input.sql);
            command.resetParameters(to_sl3_values(input.binds));
            command.execute();

            return execution_result{
                .rows_affected = database_.getRecentlyChanged(),
                .last_insert_id = database_.getLastInsertRowid(),
            };
        }
        catch(const std::exception& error)
        {
            throw_sqlite_error("execute", input.sql, error);
        }
    }

    [[nodiscard]] std::vector<row> query(const entityfy::statement& input) override
    {
        std::lock_guard lock{mutex_};

        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            std::vector<row> rows;
            auto&            command = command_for(input.sql);

            command.resetParameters(to_sl3_values(input.binds));

            command.execute(
                [&](sl3::Columns columns) {
                    row output;
                    output.reserve(static_cast<std::size_t>(columns.count()));

                    for(int index = 0; index < columns.count(); ++index)
                    {
                        output.emplace_back(from_sl3_column(columns, index));
                    }

                    rows.emplace_back(std::move(output));
                    return true;
                }
            );

            return rows;
        }
        catch(const std::exception& error)
        {
            throw_sqlite_error("query", input.sql, error);
        }
    }

    void query_each(const entityfy::statement& input,
                    const std::function<bool(const row&)>& consumer) override
    {
        std::lock_guard lock{mutex_};
        bool in_consumer = false;

        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            auto& command = command_for(input.sql);
            command.resetParameters(to_sl3_values(input.binds));

            row output;

            command.execute(
                [&](sl3::Columns columns) {
                    output.clear();

                    for(int index = 0; index < columns.count(); ++index)
                    {
                        output.emplace_back(from_sl3_column(columns, index));
                    }

                    in_consumer = true;
                    const bool keep = consumer(output);
                    in_consumer = false;
                    return keep;
                }
            );
        }
        catch(const std::exception& error)
        {
            if(in_consumer)
            {
                throw;
            }

            throw_sqlite_error("query", input.sql, error);
        }
    }

    [[nodiscard]] std::vector<std::string> table_columns(std::string_view table) override
    {
        statement input;
        input.sql = "PRAGMA table_info(" + quote_identifier(table) + ")";

        std::vector<std::string> output;
        for(const auto& result_row: query(input))
        {
            if(result_row.size() > 1)
            {
                output.emplace_back(value_to_string(result_row[1]));
            }
        }

        return output;
    }

    [[nodiscard]] table_info inspect_table(std::string_view table) override
    {
        table_info output;
        output.name = std::string{table};

        statement columns_statement;
        columns_statement.sql = "PRAGMA table_info(" + quote_identifier(table) + ")";

        for(const auto& result_row: query(columns_statement))
        {
            if(result_row.size() < 6)
            {
                continue;
            }

            // Column 5 of the pragma is the 1-based position inside the
            // primary key (0 for columns that are not part of the key).
            const auto key_ordinal = value_to_i64(result_row[5]);

            output.columns.emplace_back(column_info{
                .name = value_to_string(result_row[1]),
                .sql_type = value_to_string(result_row[2]),
                .nullable = value_to_i64(result_row[3]) == 0,
                .primary_key = key_ordinal != 0,
                .primary_key_ordinal = static_cast<int>(key_ordinal),
                .default_sql = value_to_string(result_row[4]),
            });
        }

        statement indexes_statement;
        indexes_statement.sql = "PRAGMA index_list(" + quote_identifier(table) + ")";

        for(const auto& result_row: query(indexes_statement))
        {
            if(result_row.size() < 3)
            {
                continue;
            }

            index_info index{
                .name = value_to_string(result_row[1]),
                .unique = value_to_i64(result_row[2]) != 0,
            };

            statement index_columns_statement;
            index_columns_statement.sql = "PRAGMA index_info(" + quote_identifier(index.name) + ")";

            for(const auto& index_row: query(index_columns_statement))
            {
                if(index_row.size() > 2)
                {
                    index.columns.emplace_back(value_to_string(index_row[2]));
                }
            }

            output.indexes.emplace_back(std::move(index));
        }

        statement foreign_keys_statement;
        foreign_keys_statement.sql = "PRAGMA foreign_key_list(" + quote_identifier(table) + ")";

        for(const auto& result_row: query(foreign_keys_statement))
        {
            if(result_row.size() < 7)
            {
                continue;
            }

            output.foreign_keys.emplace_back(foreign_key_info{
                .column = value_to_string(result_row[3]),
                .referenced_table = value_to_string(result_row[2]),
                .referenced_column = value_to_string(result_row[4]),
                .on_update = value_to_string(result_row[5]),
                .on_delete = value_to_string(result_row[6]),
            });
        }

        return output;
    }

    void set_statement_cache_capacity(std::size_t capacity) override
    {
        std::lock_guard lock{mutex_};

        capacity_ = capacity;

        while(cache_.size() > capacity_)
        {
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
        std::lock_guard lock{mutex_};
        cache_.clear();
        index_.clear();
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
    static constexpr std::size_t async_queue_capacity = 64;

    // One worker over a bounded FIFO queue, created on the first async call so a
    // synchronous-only user never pays for a thread. The member is declared
    // last: destruction drains and joins the worker before the statement cache
    // and the connection disappear.
    [[nodiscard]] entityfy::thread_pool_executor& async_pool()
    {
        const std::lock_guard lock{async_pool_mutex_};

        if(!async_pool_)
        {
            async_pool_ = std::make_unique<entityfy::thread_pool_executor>(1, async_queue_capacity);
        }

        return *async_pool_;
    }

    // Registers the task, its cancel hook and exactly one queued job. The job
    // calls the synchronous entry point, so SQL generation, conversion and error
    // classification stay in one place.
    template <typename T, typename Work>
        requires (!std::is_void_v<T>)
    [[nodiscard]] entityfy::task<T> offload(Work work)
    {
        auto pair = entityfy::make_task<T>();
        auto completer = std::make_shared<entityfy::task_completer<T>>(std::move(pair.completer));
        auto cancel = std::make_shared<std::atomic<bool>>(false);

        // The hook only touches the shared flag, so a cancellation that races
        // the backend's destruction never reaches the backend itself.
        completer->set_cancel_hook([cancel] { cancel->store(true, std::memory_order_release); });

        try
        {
            async_pool().submit([work = std::move(work), completer, cancel]() mutable {
                // Cooperative cancellation: a queued job that was cancelled
                // never touches SQLite. A statement that already started runs to
                // completion, because one SQLite step cannot be interrupted.
                if(cancel->load(std::memory_order_acquire))
                {
                    completer->set_cancelled();
                    return;
                }

                try
                {
                    completer->set_value(work());
                }
                catch(...)
                {
                    completer->set_exception(std::current_exception());
                }
            });
        }
        catch(...)
        {
            completer->set_exception(std::current_exception());
        }

        return std::move(pair.value);
    }

    // Least recently used cache of compiled statements. An unordered_map gives
    // O(1) lookup; ordering is kept in a list (front = oldest, back = newest).
    [[nodiscard]] sl3::Command& command_for(const std::string& sql)
    {
        if(capacity_ == 0)
        {
            scratch_ = std::make_unique<sl3::Command>(database_.prepare(sql));
            return *scratch_;
        }

        if(const auto found = index_.find(sql); found != index_.end())
        {
            cache_.splice(cache_.begin(), cache_, found->second);
            return *cache_.front().second;
        }

        auto command = std::make_unique<sl3::Command>(database_.prepare(sql));
        auto* raw = command.get();

        cache_.emplace_front(sql, std::move(command));
        index_[sql] = cache_.begin();

        while(cache_.size() > capacity_)
        {
            index_.erase(cache_.back().first);
            cache_.pop_back();
        }

        return *raw;
    }

    sl3::Database database_;
    mutable std::recursive_mutex                      mutex_;
    std::size_t                                      capacity_ = 32;
    std::size_t                                      batch_parameter_limit_ = 900;
    std::function<void(std::string_view)>            logger_;
    std::list<std::pair<std::string, std::unique_ptr<sl3::Command>>> cache_;
    std::unordered_map<std::string, decltype(cache_.begin())> index_;
    std::unique_ptr<sl3::Command>                    scratch_;

    // The pool is declared after every member its jobs use, so its drain+join
    // runs while the cache, the mutex and the connection are still alive, and
    // before the connection is closed.
    std::mutex                                       async_pool_mutex_;
    std::unique_ptr<entityfy::thread_pool_executor>  async_pool_;
};

std::unique_ptr<backend> make_sqlite_backend(std::string path)
{
    return std::make_unique<sqlite_backend>(std::move(path));
}

} // namespace entityfy::detail
