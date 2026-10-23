#include <catch2/catch_test_macros.hpp>

#include <entityfy/client.hpp>
#include <entityfy/detail/backend.hpp>
#include <entityfy/task.hpp>
#include <entityfy/value.hpp>

#include <sqlite3.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// SQLite is the always-on backend of the default build: its async surface
// offloads the synchronous path to one bounded worker, so these cases run
// unconditionally when SQLite is compiled in.
#ifdef ENTITYFY_ENABLE_SQLITE

namespace
{

using entityfy::detail::execution_result;
using entityfy::detail::row;
using entityfy::detail::value_to_i64;
using entityfy::detail::value_to_string;
using entityfy::statement;

// Same temp-file pattern as the other SQLite tests: a file database keeps the
// async connection comparable with the synchronous one instead of relying on a
// private in-memory connection.
class temporary_database
{
public:
    explicit temporary_database(std::string_view name)
        : path_{(std::filesystem::temp_directory_path() /
                 ("entityfy_sqlite_async_" + std::string{name} + ".db")).string()}
    {
        std::filesystem::remove(path_);
    }

    ~temporary_database()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    temporary_database(const temporary_database&) = delete;
    temporary_database& operator=(const temporary_database&) = delete;

    [[nodiscard]] const std::string& path() const noexcept
    {
        return path_;
    }

    [[nodiscard]] std::string uri() const
    {
        return "sqlite://" + path_;
    }

private:
    std::string path_;
};

[[nodiscard]] std::string insert_sql(std::string_view table)
{
    return "INSERT INTO \"" + std::string{table} + "\" (\"id\", \"name\") VALUES (?, ?)";
}

[[nodiscard]] statement insert_statement(std::string_view table, std::int64_t id, std::string name)
{
    return statement{
        .sql = insert_sql(table),
        .binds = {id, std::move(name)},
    };
}

// Opens a second connection so a test can hold a write lock while the worker is
// inside an async statement. Closing the handle rolls back the transaction.
class external_connection
{
public:
    explicit external_connection(const std::string& path)
    {
        if(sqlite3_open(path.c_str(), &handle_) != SQLITE_OK)
        {
            const std::string message = handle_ == nullptr ? "sqlite3_open failed" : sqlite3_errmsg(handle_);
            sqlite3_close(handle_);
            handle_ = nullptr;
            throw std::runtime_error{message};
        }
    }

    ~external_connection()
    {
        if(handle_ != nullptr)
        {
            sqlite3_close(handle_);
        }
    }

    external_connection(const external_connection&) = delete;
    external_connection& operator=(const external_connection&) = delete;

    [[nodiscard]] sqlite3* get() const noexcept
    {
        return handle_;
    }

private:
    sqlite3* handle_ = nullptr;
};

// Gate that waits for the worker to reach a statement instead of sleeping. The
// logger runs inside the backend mutex before SQLite is touched, so once it has
// fired the worker is provably busy with that statement.
class statement_gate
{
public:
    explicit statement_gate(std::string marker)
        : marker_{std::move(marker)}
    {
    }

    void signal(std::string_view sql)
    {
        if(sql.find(marker_) == std::string_view::npos)
        {
            return;
        }

        {
            const std::lock_guard lock{mutex_};
            if(open_)
            {
                return;
            }

            open_ = true;
        }

        cv_.notify_all();
    }

    void wait()
    {
        std::unique_lock lock{mutex_};
        cv_.wait(lock, [this] { return open_; });
    }

private:
    std::string             marker_;
    std::mutex              mutex_;
    std::condition_variable cv_;
    bool                    open_ = false;
};

} // namespace

TEST_CASE("sqlite async capability is thread offload", "[sqlite][async]")
{
    temporary_database file{"capability"};
    entityfy::client db{file.uri()};

    REQUIRE(db.async_mode() == entityfy::async_capability::thread_offload);
}

TEST_CASE("sqlite async execute and query deliver the synchronous results", "[sqlite][async]")
{
    temporary_database file{"execute_query"};
    entityfy::client db{file.uri()};

    const auto created = db.execute_async(statement{
        .sql = "CREATE TABLE \"async_items\" (\"id\" INTEGER PRIMARY KEY AUTOINCREMENT, \"name\" TEXT NOT NULL)"
    }).get();

    REQUIRE(created.rows_affected == 0);

    const auto first = db.execute_async(insert_statement("async_items", 1, "first")).get();
    REQUIRE(first.rows_affected == 1);
    REQUIRE(first.last_insert_id == 1);

    const auto second = db.execute_async(insert_statement("async_items", 2, "second")).get();
    REQUIRE(second.rows_affected == 1);
    REQUIRE(second.last_insert_id == 2);

    const statement select{
        .sql = "SELECT \"id\", \"name\" FROM \"async_items\" ORDER BY \"id\"",
    };

    const std::vector<row> asynchronous = db.query_async(select).get();
    const std::vector<row> synchronous = db.query(select);

    REQUIRE(asynchronous == synchronous);
    REQUIRE(asynchronous.size() == 2);
    REQUIRE(asynchronous[0].size() == 2);
    REQUIRE(value_to_i64(asynchronous[0][0]) == 1);
    REQUIRE(value_to_string(asynchronous[0][1]) == "first");
    REQUIRE(value_to_i64(asynchronous[1][0]) == 2);
    REQUIRE(value_to_string(asynchronous[1][1]) == "second");
}

TEST_CASE("sqlite async batch matches the synchronous batch", "[sqlite][async][batch]")
{
    temporary_database file{"batch"};

    // The client exposes only the asynchronous batch, so the synchronous
    // counterpart is exercised on the same backend directly.
    const auto db = entityfy::detail::make_sqlite_backend(file.path());

    (void)db->execute(statement{
        .sql = "CREATE TABLE \"async_batch\" (\"id\" INTEGER PRIMARY KEY AUTOINCREMENT, \"name\" TEXT NOT NULL)"
    });

    std::vector<statement> synchronous;
    std::vector<statement> asynchronous;

    for(std::int64_t index = 0; index < 3; ++index)
    {
        synchronous.push_back(insert_statement(
            "async_batch",
            index + 1,
            "sync " + std::to_string(index)
        ));
        asynchronous.push_back(insert_statement(
            "async_batch",
            index + 11,
            "async " + std::to_string(index)
        ));
    }

    const std::vector<execution_result> synchronous_results = db->execute_batch(synchronous);
    const std::vector<execution_result> asynchronous_results = db->execute_batch_async(asynchronous).get();

    REQUIRE(synchronous_results.size() == 3);
    REQUIRE(asynchronous_results.size() == synchronous_results.size());

    for(std::size_t index = 0; index < synchronous_results.size(); ++index)
    {
        REQUIRE(asynchronous_results[index].rows_affected == synchronous_results[index].rows_affected);
        REQUIRE(asynchronous_results[index].rows_affected == 1);
    }

    const auto counted = db->query_async(statement{
        .sql = "SELECT count(*) FROM \"async_batch\"",
    }).get();

    REQUIRE(counted.size() == 1);
    REQUIRE(value_to_i64(counted.front().front()) == 6);
}

TEST_CASE("sqlite async jobs run in submission order", "[sqlite][async][fifo]")
{
    temporary_database file{"fifo"};
    entityfy::client db{file.uri()};

    (void)db.execute(statement{
        .sql = "CREATE TABLE \"async_fifo\" (\"id\" INTEGER PRIMARY KEY AUTOINCREMENT, \"name\" TEXT NOT NULL)"
    });

    std::vector<entityfy::task<execution_result>> pending;
    constexpr std::int64_t job_count = 8;

    for(std::int64_t index = 0; index < job_count; ++index)
    {
        pending.push_back(db.execute_async(insert_statement(
            "async_fifo",
            index + 1,
            "row " + std::to_string(index)
        )));
    }

    // One worker over a FIFO queue: results arrive in submission order and each
    // insert sees the previous one committed.
    for(std::int64_t index = 0; index < job_count; ++index)
    {
        const auto result = pending[static_cast<std::size_t>(index)].get();
        REQUIRE(result.rows_affected == 1);
        REQUIRE(result.last_insert_id == index + 1);
    }

    const auto rows = db.query(statement{
        .sql = "SELECT \"id\", \"name\" FROM \"async_fifo\" ORDER BY \"id\"",
    });

    REQUIRE(rows.size() == static_cast<std::size_t>(job_count));

    for(std::int64_t index = 0; index < job_count; ++index)
    {
        REQUIRE(value_to_i64(rows[static_cast<std::size_t>(index)][0]) == index + 1);
        REQUIRE(value_to_string(rows[static_cast<std::size_t>(index)][1]) == "row " + std::to_string(index));
    }

    // The connection stays usable for both paths after the queue drained.
    const auto after = db.execute_async(insert_statement("async_fifo", 100, "after")).get();
    REQUIRE(after.rows_affected == 1);

    REQUIRE(value_to_i64(db.query(statement{
        .sql = "SELECT count(*) FROM \"async_fifo\"",
    }).front().front()) == job_count + 1);
}

TEST_CASE("sqlite async cancellation is cooperative", "[sqlite][async][cancel]")
{
    temporary_database file{"cancel"};
    entityfy::client db{file.uri()};

    (void)db.execute(statement{
        .sql = "CREATE TABLE \"async_cancel\" (\"id\" INTEGER PRIMARY KEY, \"name\" TEXT NOT NULL)"
    });
    (void)db.execute(insert_statement("async_cancel", 1, "committed"));

    // A cancellation requested after the value was published never reaches the
    // producer, so the delivered value is unchanged.
    std::promise<void> published;
    auto finished = db.query_async(statement{
        .sql = "SELECT \"name\" FROM \"async_cancel\" WHERE \"id\" = 1",
    });

    finished.on_complete([&published] { published.set_value(); });
    published.get_future().wait();
    finished.request_cancel();

    const auto finished_rows = finished.get();
    REQUIRE(finished_rows.size() == 1);
    REQUIRE(value_to_string(finished_rows.front().front()) == "committed");

    // Hold a write lock from a second connection. The async INSERT can prepare
    // but its step waits on the busy handler, so the single worker is provably
    // busy while the next task is queued.
    statement_gate gate{insert_sql("async_cancel")};
    db.set_logger([&gate](std::string_view sql) { gate.signal(sql); });

    external_connection other{file.path()};
    REQUIRE(sqlite3_exec(other.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);

    auto blocked = db.execute_async(insert_statement("async_cancel", 2, "blocked"));
    gate.wait();

    auto queued = db.execute_async(insert_statement("async_cancel", 3, "cancel queued"));
    queued.request_cancel();

    REQUIRE(sqlite3_exec(other.get(), "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK);

    const auto blocked_result = blocked.get();
    REQUIRE(blocked_result.rows_affected == 1);

    // The queued job observed the flag before it started, so it never touched
    // SQLite. A statement already inside SQLite cannot be interrupted, which is
    // why the blocked task above still delivered its value.
    REQUIRE_THROWS_AS(queued.get(), entityfy::task_cancelled);

    // The cancellation left the connection healthy: only the two statements
    // that actually ran are visible.
    const auto rows = db.query_async(statement{
        .sql = "SELECT \"id\" FROM \"async_cancel\" ORDER BY \"id\"",
    }).get();

    REQUIRE(rows.size() == 2);
    REQUIRE(value_to_i64(rows[0][0]) == 1);
    REQUIRE(value_to_i64(rows[1][0]) == 2);
}

TEST_CASE("sqlite sync and async calls interleave safely", "[sqlite][async][interleave]")
{
    temporary_database file{"interleave"};
    entityfy::client db{file.uri()};

    (void)db.execute(statement{
        .sql = "CREATE TABLE \"async_interleave\" (\"id\" INTEGER PRIMARY KEY, \"name\" TEXT NOT NULL)"
    });

    const auto asynchronous_insert = db.execute_async(insert_statement("async_interleave", 1, "async")).get();
    REQUIRE(asynchronous_insert.rows_affected == 1);

    const auto synchronous_insert = db.execute(insert_statement("async_interleave", 2, "sync"));
    REQUIRE(synchronous_insert.rows_affected == 1);

    const statement select{
        .sql = "SELECT \"id\", \"name\" FROM \"async_interleave\" ORDER BY \"id\"",
    };

    const std::vector<row> asynchronous_rows = db.query_async(select).get();
    const std::vector<row> synchronous_rows = db.query(select);

    REQUIRE(asynchronous_rows == synchronous_rows);
    REQUIRE(asynchronous_rows.size() == 2);
    REQUIRE(value_to_string(asynchronous_rows[0][1]) == "async");
    REQUIRE(value_to_string(asynchronous_rows[1][1]) == "sync");

    // A synchronous query between two async calls still sees a consistent
    // connection.
    const auto counted = db.query_async(statement{
        .sql = "SELECT count(*) FROM \"async_interleave\"",
    }).get();

    REQUIRE(value_to_i64(counted.front().front()) == 2);
    REQUIRE(db.execute(insert_statement("async_interleave", 3, "sync after async")).rows_affected == 1);

    const auto final_rows = db.query_async(select).get();
    REQUIRE(final_rows == db.query(select));
    REQUIRE(final_rows.size() == 3);
}

#endif // ENTITYFY_ENABLE_SQLITE
