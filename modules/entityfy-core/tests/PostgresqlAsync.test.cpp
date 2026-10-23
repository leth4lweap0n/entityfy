#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <entityfy/detail/backend.hpp>
#include <entityfy/error.hpp>
#include <entityfy/task.hpp>
#include <entityfy/value.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The PostgreSQL backend is an optional component; the suite only compiles when
// the transport is enabled, and it self-gates on ENTITYFY_PG_URI otherwise.
#ifdef ENTITYFY_ENABLE_POSTGRESQL

namespace
{

using entityfy::detail::async_capability;
using entityfy::detail::execution_result;
using entityfy::detail::row;
using entityfy::detail::value_to_i64;
using entityfy::detail::value_to_string;
using entityfy::statement;

// A test run without a PostgreSQL server skips, but a run that explicitly
// requires PostgreSQL fails instead of quietly skipping: a green
// ENTITYFY_REQUIRE_PG matrix has to mean the statements below really ran.
[[nodiscard]] std::string postgres_uri()
{
    const char* uri = std::getenv("ENTITYFY_PG_URI");

    if(uri == nullptr || *uri == '\0')
    {
        if(std::getenv("ENTITYFY_REQUIRE_PG") != nullptr)
        {
            throw std::runtime_error{"ENTITYFY_REQUIRE_PG is set but ENTITYFY_PG_URI is missing"};
        }

        return {};
    }

    return std::string{uri};
}

[[nodiscard]] std::string insert_sql_for(const std::string& table)
{
    return "INSERT INTO \"" + table + "\" (\"id\", \"name\") VALUES (?, ?)";
}

// Gate that the tests use to wait for the reactor thread instead of sleeping.
class completion_gate
{
public:
    void open(std::thread::id thread)
    {
        {
            const std::lock_guard lock{mutex_};
            thread_ = thread;
            open_ = true;
        }

        cv_.notify_all();
    }

    [[nodiscard]] std::thread::id wait()
    {
        std::unique_lock lock{mutex_};
        cv_.wait(lock, [this] { return open_; });

        return thread_;
    }

private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::thread::id         thread_{};
    bool                    open_ = false;
};

} // namespace

TEST_CASE("postgres reports native async capability", "[postgresql][async]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    const auto db = entityfy::detail::make_postgresql_backend(uri);

    REQUIRE(db->async_mode() == async_capability::native);
    REQUIRE(db->target_dialect() == entityfy::dialect::postgresql);
}

TEST_CASE("postgres async execute and query deliver the synchronous results", "[postgresql][async]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    const auto db = entityfy::detail::make_postgresql_backend(uri);

    db->execute(statement{.sql = "DROP TABLE IF EXISTS \"pg_async_items\""});

    const auto created = db->execute_async(statement{
        .sql = "CREATE TABLE \"pg_async_items\" (\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL)"
    }).get();
    REQUIRE(created.rows_affected == 0);

    const auto inserted = db->execute_async(statement{
        .sql = insert_sql_for("pg_async_items"),
        .binds = {std::int64_t{1}, std::string{"first"}},
    }).get();
    REQUIRE(inserted.rows_affected == 1);

    db->execute_async(statement{
        .sql = insert_sql_for("pg_async_items"),
        .binds = {std::int64_t{2}, std::string{"second"}},
    }).get();

    const auto rows = db->query_async(statement{
        .sql = "SELECT \"id\", \"name\" FROM \"pg_async_items\" ORDER BY \"id\""
    }).get();

    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].size() == 2);
    REQUIRE(value_to_i64(rows[0][0]) == 1);
    REQUIRE(value_to_string(rows[0][1]) == "first");
    REQUIRE(value_to_i64(rows[1][0]) == 2);
    REQUIRE(value_to_string(rows[1][1]) == "second");

    // Continuations run on the thread that completed the task and keep chaining
    // through further tasks.
    auto counted = db->query_async(statement{
        .sql = "SELECT \"id\" FROM \"pg_async_items\""
    }).then([](std::vector<row> loaded) {
        return static_cast<std::int64_t>(loaded.size());
    });

    REQUIRE(counted.get() == 2);

    db->execute(statement{.sql = "DROP TABLE IF EXISTS \"pg_async_items\""});
}

TEST_CASE("postgres async batch matches the synchronous batch", "[postgresql][async][batch]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    const auto db = entityfy::detail::make_postgresql_backend(uri);

    db->execute(statement{.sql = "DROP TABLE IF EXISTS \"pg_async_batch\""});
    db->execute(statement{
        .sql = "CREATE TABLE \"pg_async_batch\" (\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL)"
    });

    std::vector<statement> synchronous;
    std::vector<statement> asynchronous;

    for(std::int64_t index = 0; index < 3; ++index)
    {
        synchronous.push_back(statement{
            .sql = insert_sql_for("pg_async_batch"),
            .binds = {index + 1, "sync " + std::to_string(index)},
        });
        asynchronous.push_back(statement{
            .sql = insert_sql_for("pg_async_batch"),
            .binds = {index + 11, "async " + std::to_string(index)},
        });
    }

    const std::vector<execution_result> synchronous_results = db->execute_batch(synchronous);
    const std::vector<execution_result> asynchronous_results = db->execute_batch_async(asynchronous).get();

    REQUIRE(synchronous_results.size() == 3);
    REQUIRE(asynchronous_results.size() == 3);

    for(std::size_t index = 0; index < synchronous_results.size(); ++index)
    {
        REQUIRE(asynchronous_results[index].rows_affected == synchronous_results[index].rows_affected);
        REQUIRE(asynchronous_results[index].rows_affected == 1);
    }

    const auto counted = db->query_async(statement{
        .sql = "SELECT count(*) FROM \"pg_async_batch\""
    }).get();

    REQUIRE(counted.size() == 1);
    REQUIRE(value_to_i64(counted.front().front()) == 6);

    db->execute(statement{.sql = "DROP TABLE IF EXISTS \"pg_async_batch\""});
}

TEST_CASE("postgres allows one async operation in flight per connection", "[postgresql][async][single-flight]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    const auto db = entityfy::detail::make_postgresql_backend(uri);

    auto slow = db->query_async(statement{.sql = "SELECT pg_sleep(0.2)"});

    REQUIRE_THROWS_AS(db->query_async(statement{.sql = "SELECT 1"}), std::logic_error);
    REQUIRE_THROWS_AS(db->execute_async(statement{.sql = "SELECT 1"}), std::logic_error);
    REQUIRE_THROWS_AS(
        db->execute_batch_async({statement{.sql = "SELECT 1"}, statement{.sql = "SELECT 2"}}),
        std::logic_error
    );

    REQUIRE_THROWS_WITH(
        db->query_async(statement{.sql = "SELECT 1"}),
        Catch::Matchers::ContainsSubstring("one async operation at a time")
    );

    // The synchronous entry points reject instead of blocking on the busy
    // connection.
    REQUIRE_THROWS_WITH(db->query(statement{.sql = "SELECT 1"}), "connection busy with an async operation");
    REQUIRE_THROWS_WITH(db->execute(statement{.sql = "SELECT 1"}), "connection busy with an async operation");
    REQUIRE_THROWS_WITH(
        db->execute_batch(std::vector<statement>{statement{.sql = "SELECT 1"}}),
        "connection busy with an async operation"
    );
    REQUIRE_THROWS_WITH(
        db->query_each(statement{.sql = "SELECT 1"}, [](const row&) { return true; }),
        "connection busy with an async operation"
    );

    const auto rows = slow.get();
    REQUIRE(rows.size() == 1);

    // The connection serves both paths again once the in-flight operation is
    // complete.
    REQUIRE(db->query(statement{.sql = "SELECT 1"}).size() == 1);
    REQUIRE(db->query_async(statement{.sql = "SELECT 1"}).get().size() == 1);
}

TEST_CASE("postgres async cancellation aborts the statement", "[postgresql][async][cancel]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    const auto db = entityfy::detail::make_postgresql_backend(uri);

    auto pending = db->query_async(statement{.sql = "SELECT pg_sleep(10)"});

    const auto started = std::chrono::steady_clock::now();
    pending.request_cancel();

    REQUIRE_THROWS_AS(pending.get(), entityfy::task_cancelled);

    // Only the cancel hook raises the flag the reactor polls, and only the
    // reactor's cancel request can cut the ten second server side sleep short:
    // a fast task_cancelled is the proof that the hook ran and that the server
    // abort (SQLSTATE 57014) was observed.
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    REQUIRE(elapsed < 5.0);

    // The aborted statement was drained, so the connection is usable again.
    REQUIRE(db->query(statement{.sql = "SELECT 1"}).size() == 1);
    REQUIRE(db->query_async(statement{.sql = "SELECT 1"}).get().size() == 1);

    // A cancel request after the value was published is a no-op.
    completion_gate published;
    auto finished = db->query_async(statement{.sql = "SELECT 42"});

    finished.on_complete([&published] { published.open(std::this_thread::get_id()); });
    (void)published.wait();

    finished.request_cancel();
    REQUIRE(value_to_i64(finished.get().front().front()) == 42);
}

TEST_CASE("postgres async pipeline failure is classified and drained", "[postgresql][async][batch]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    const auto db = entityfy::detail::make_postgresql_backend(uri);

    db->execute(statement{.sql = "DROP TABLE IF EXISTS \"pg_async_pipeline\""});
    db->execute(statement{
        .sql = "CREATE TABLE \"pg_async_pipeline\" (\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL)"
    });

    const std::vector<statement> batch{
        statement{.sql = insert_sql_for("pg_async_pipeline"), .binds = {std::int64_t{1}, std::string{"a"}}},
        statement{.sql = insert_sql_for("pg_async_pipeline"), .binds = {std::int64_t{2}, std::string{"b"}}},
        statement{.sql = insert_sql_for("pg_async_pipeline"), .binds = {std::int64_t{1}, std::string{"dup"}}},
        statement{.sql = insert_sql_for("pg_async_pipeline"), .binds = {std::int64_t{3}, std::string{"c"}}},
    };

    REQUIRE_THROWS_AS(db->execute_batch_async(batch).get(), entityfy::unique_violation);

    // A pipeline is one implicit transaction, so the failure rolls the whole
    // batch back; every result up to the pipeline sync was still consumed, so
    // the connection serves both paths again.
    const auto rows = db->query_async(statement{
        .sql = "SELECT \"id\" FROM \"pg_async_pipeline\" ORDER BY \"id\""
    }).get();

    REQUIRE(rows.empty());

    db->execute(statement{
        .sql = insert_sql_for("pg_async_pipeline"),
        .binds = {std::int64_t{7}, std::string{"after failure"}},
    });

    const auto after_failure = db->query(statement{
        .sql = "SELECT \"id\" FROM \"pg_async_pipeline\""
    });

    REQUIRE(after_failure.size() == 1);
    REQUIRE(value_to_i64(after_failure.front().front()) == 7);

    db->execute(statement{.sql = "DROP TABLE IF EXISTS \"pg_async_pipeline\""});
}

TEST_CASE("postgres async completion runs on the reactor thread", "[postgresql][async][thread]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    const auto db = entityfy::detail::make_postgresql_backend(uri);
    const auto caller = std::this_thread::get_id();

    auto pending = db->query_async(statement{.sql = "SELECT pg_sleep(0.2)"});

    completion_gate gate;
    pending.on_complete([&gate] { gate.open(std::this_thread::get_id()); });

    REQUIRE(gate.wait() != caller);
    REQUIRE(pending.get().size() == 1);
}

TEST_CASE("postgres async task survives the backend destruction", "[postgresql][async][lifetime]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    auto pending = [&uri] {
        const auto db = entityfy::detail::make_postgresql_backend(uri);
        return db->query_async(statement{.sql = "SELECT pg_sleep(0.1)"});
    }();

    // The destructor drained the queue and joined the reactor before the
    // connection went away, so the queued job still completed its task.
    REQUIRE(pending.get().size() == 1);
}

TEST_CASE("postgres COPY is reported instead of hanging the async drain", "[postgresql][async][copy]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    const auto db = entityfy::detail::make_postgresql_backend(uri);

    // libpq keeps answering every result call in copy state with a fresh COPY
    // result, so a regressed drain spins inside the reactor and never completes
    // the task. The watchdog turns that into a failed run instead of a test
    // that hangs, and the backend is never destroyed in that state.
    std::atomic<bool> finished{false};
    std::thread       watchdog{[&finished] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};

        while(std::chrono::steady_clock::now() < deadline)
        {
            if(finished.load(std::memory_order_acquire))
            {
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        std::fprintf(stderr, "the COPY statement never completed: the result drain is spinning\n");
        std::_Exit(EXIT_FAILURE);
    }};

    bool async_failed = false;
    bool sync_failed = false;

    try
    {
        db->query_async(statement{.sql = "COPY (SELECT 1) TO STDOUT"}).get();
    }
    catch(const entityfy::query_error&)
    {
        async_failed = true;
    }

    try
    {
        db->execute(statement{.sql = "COPY (SELECT 1) TO STDOUT"});
    }
    catch(const entityfy::query_error&)
    {
        sync_failed = true;
    }

    // The copy stream was discarded, so both paths serve the connection again
    // instead of failing with "connection in COPY state".
    const auto synchronous = db->query(statement{.sql = "SELECT 1"});
    const auto asynchronous = db->query_async(statement{.sql = "SELECT 1"}).get();

    finished.store(true, std::memory_order_release);
    watchdog.join();

    REQUIRE(async_failed);
    REQUIRE(sync_failed);
    REQUIRE(synchronous.size() == 1);
    REQUIRE(asynchronous.size() == 1);
}

TEST_CASE("postgres async retries plainly when the prepared plan disappeared", "[postgresql][async][cache]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    const auto db = entityfy::detail::make_postgresql_backend(uri);

    db->execute(statement{.sql = "DROP TABLE IF EXISTS \"pg_async_stale_plan\""});
    db->execute(statement{.sql = "CREATE TABLE \"pg_async_stale_plan\" (\"id\" BIGINT PRIMARY KEY)"});

    const std::string sql = "SELECT count(*) FROM \"pg_async_stale_plan\"";

    // The synchronous path prepares and caches the plan.
    REQUIRE(db->query(statement{.sql = sql}).size() == 1);

    const auto prepared = db->query(statement{
        .sql = "SELECT count(*) FROM pg_prepared_statements WHERE statement = 'SELECT count(*) FROM \"pg_async_stale_plan\"'"
    });

    REQUIRE(prepared.size() == 1);
    REQUIRE(value_to_i64(prepared.front().front()) == 1);

    // The server forgets every plan while the client still holds the name, so
    // the next prepared call fails with SQLSTATE 26000.
    db->execute(statement{.sql = "DEALLOCATE ALL"});

    // The async path drops the dead name and retries unprepared instead of
    // surfacing the stale name error.
    const auto rows = db->query_async(statement{.sql = sql}).get();

    REQUIRE(rows.size() == 1);
    REQUIRE(value_to_i64(rows.front().front()) == 0);

    // The connection is usable afterwards, through both paths.
    REQUIRE(db->query_async(statement{.sql = sql}).get().size() == 1);
    REQUIRE(db->query(statement{.sql = sql}).size() == 1);

    db->execute(statement{.sql = "DROP TABLE IF EXISTS \"pg_async_stale_plan\""});
}

TEST_CASE("postgres async cancel requested before the send stays consistent", "[postgresql][async][cancel]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    const auto db = entityfy::detail::make_postgresql_backend(uri);

    db->execute(statement{.sql = "DROP TABLE IF EXISTS \"pg_async_early_cancel\""});
    db->execute(statement{
        .sql = "CREATE TABLE \"pg_async_early_cancel\" (\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL)"
    });

    auto pending = db->execute_async(statement{
        .sql = insert_sql_for("pg_async_early_cancel"),
        .binds = {std::int64_t{1}, std::string{"early"}},
    });

    // The cancel races the reactor on purpose: a cancel that wins the race must
    // send nothing at all, one that arrives later either aborts or misses the
    // statement. Both outcomes are valid, an inconsistent row is not.
    pending.request_cancel();

    bool cancelled = false;
    try
    {
        (void)pending.get();
    }
    catch(const entityfy::task_cancelled&)
    {
        cancelled = true;
    }

    const auto counted = db->query(statement{
        .sql = "SELECT count(*) FROM \"pg_async_early_cancel\""
    });

    REQUIRE(counted.size() == 1);
    REQUIRE(value_to_i64(counted.front().front()) == (cancelled ? 0 : 1));

    // The connection serves both paths again after the cancelled operation.
    REQUIRE(db->query(statement{.sql = "SELECT 1"}).size() == 1);
    REQUIRE(db->query_async(statement{.sql = "SELECT 1"}).get().size() == 1);

    db->execute(statement{.sql = "DROP TABLE IF EXISTS \"pg_async_early_cancel\""});
}

TEST_CASE("postgres async pipeline does not wait per statement", "[postgresql][async][batch][latency]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    const auto db = entityfy::detail::make_postgresql_backend(uri);

    db->execute(statement{.sql = "DROP TABLE IF EXISTS \"pg_async_pipeline_latency\""});
    db->execute(statement{
        .sql = "CREATE TABLE \"pg_async_pipeline_latency\" (\"id\" BIGINT PRIMARY KEY, \"name\" TEXT NOT NULL)"
    });

    std::vector<statement> batch;
    batch.reserve(40);

    for(std::int64_t index = 0; index < 40; ++index)
    {
        batch.push_back(statement{
            .sql = insert_sql_for("pg_async_pipeline_latency"),
            .binds = {index + 1, "row " + std::to_string(index)},
        });
    }

    const auto started = std::chrono::steady_clock::now();
    const auto results = db->execute_batch_async(batch).get();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    REQUIRE(results.size() == batch.size());

    // Polling the socket after every per-query null costs one poll interval per
    // statement, which puts forty statements over two seconds; reading the
    // buffered results costs one round trip. The bound leaves plenty of room
    // for a slow machine while still catching the regression.
    REQUIRE(elapsed < 1.5);

    db->execute(statement{.sql = "DROP TABLE IF EXISTS \"pg_async_pipeline_latency\""});
}

TEST_CASE("postgres async keeps a server side 57014 classified as an error", "[postgresql][async][cancel]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    const auto db = entityfy::detail::make_postgresql_backend(uri);

    // The server aborts on its own (statement_timeout) without any cancel
    // request from entityfy, so SQLSTATE 57014 has to keep its query
    // classification instead of being reported as a cancellation.
    db->execute(statement{.sql = "SET statement_timeout = '50ms'"});

    REQUIRE_THROWS_AS(db->query_async(statement{.sql = "SELECT pg_sleep(1)"}).get(), entityfy::query_error);

    db->execute(statement{.sql = "SET statement_timeout = 0"});

    // The aborted statement was drained, so the connection still works.
    REQUIRE(db->query(statement{.sql = "SELECT 1"}).size() == 1);
    REQUIRE(db->query_async(statement{.sql = "SELECT 1"}).get().size() == 1);
}

#endif // ENTITYFY_ENABLE_POSTGRESQL
