#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <entityfy/client.hpp>
#include <entityfy/table.hpp>
#include <entityfy/value.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The async surface is only useful when a backend implements it, so the client
// level cases below run against PostgreSQL; the SQLite case proves that a
// backend without async support rejects instead of falling back silently.

namespace
{

struct [[= entityfy::table{"async_client_items"}]] async_item
{
    // Explicit key: the PostgreSQL backend does not report a generated key on
    // insert (the sync path leaves last_insert_id at zero), so the tests pin the
    // identity themselves and stay comparable between both paths.
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::varchar{80}]]
    std::string name;

    [[= entityfy::not_null]]
    std::int64_t score = 0;
};

// A run without a PostgreSQL server skips, but a run that explicitly requires
// PostgreSQL fails instead of quietly skipping: a green ENTITYFY_REQUIRE_PG
// matrix has to mean the statements below really ran.
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

[[nodiscard]] entityfy::row_key item_key(std::int64_t id)
{
    return entityfy::row_key{entityfy::sql_value{id}};
}

} // namespace

#ifdef ENTITYFY_ENABLE_SQLITE

TEST_CASE("sqlite client reports thread-offload async capability", "[async][client][sqlite]")
{
    entityfy::client db{"sqlite://:memory:"};

    REQUIRE(db.async_mode() == entityfy::async_capability::thread_offload);

    const auto rows = db.query_async(entityfy::statement{.sql = "SELECT 1"}).get();
    REQUIRE(rows.size() == 1);
    REQUIRE(rows.front().size() == 1);

    const auto batch = db.execute_batch_async(std::vector<entityfy::statement>{
        entityfy::statement{.sql = "SELECT 1"},
        entityfy::statement{.sql = "SELECT 2"},
    }).get();
    REQUIRE(batch.size() == 2);
}

TEST_CASE("sqlite client transaction_async commits and rolls back on its worker", "[async][client][sqlite]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<async_item>();

    std::thread::id body_thread;
    db.transaction_async([&](entityfy::client& tx) {
        body_thread = std::this_thread::get_id();
        tx.table<async_item>().insert(async_item{.id = 1, .name = "committed", .score = 1});
    }).get();

    // The body really ran off the calling thread, and its insert committed.
    REQUIRE(body_thread != std::this_thread::get_id());
    REQUIRE(db.table<async_item>().count() == 1);
    REQUIRE(db.table<async_item>().find(std::int64_t{1})->name == "committed");

    auto failing = db.transaction_async([&]() -> std::int64_t {
        db.table<async_item>().insert(async_item{.id = 2, .name = "rolled back", .score = 2});
        throw std::runtime_error{"sqlite transaction_async body failed"};
    });

    REQUIRE_THROWS_WITH(failing.get(), "sqlite transaction_async body failed");
    REQUIRE(db.table<async_item>().count() == 1);
    REQUIRE_FALSE(db.table<async_item>().find(std::int64_t{2}).has_value());

    // The rollback released the connection: another async transaction commits.
    REQUIRE(db.transaction_async([&](entityfy::client& tx) {
        return tx.table<async_item>().insert(async_item{.id = 3, .name = "after rollback", .score = 3}).rows_affected;
    }).get() == 1);
    REQUIRE(db.table<async_item>().count() == 2);
}

TEST_CASE("sqlite client transaction_async cancels a queued transaction before it starts", "[async][client][sqlite]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<async_item>();

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool body_started = false;
    bool release_body = false;

    // The only worker is busy until the main thread releases the gate, so the
    // second transaction is still queued when it is cancelled.
    auto first = db.transaction_async([&](entityfy::client& tx) {
        {
            const std::lock_guard lock{gate_mutex};
            body_started = true;
        }
        gate_cv.notify_all();

        {
            std::unique_lock lock{gate_mutex};
            gate_cv.wait(lock, [&] { return release_body; });
        }

        tx.table<async_item>().insert(async_item{.id = 1, .name = "kept", .score = 1});
    });

    {
        std::unique_lock lock{gate_mutex};
        gate_cv.wait(lock, [&] { return body_started; });
    }

    auto queued = db.transaction_async([&](entityfy::client& tx) {
        tx.table<async_item>().insert(async_item{.id = 2, .name = "cancelled", .score = 2});
    });
    queued.request_cancel();

    {
        const std::lock_guard lock{gate_mutex};
        release_body = true;
    }
    gate_cv.notify_all();

    first.get();
    REQUIRE_THROWS_AS(queued.get(), entityfy::task_cancelled);

    // The started transaction still committed; the cancelled one never touched
    // the connection.
    REQUIRE(db.table<async_item>().count() == 1);
    REQUIRE(db.table<async_item>().find(std::int64_t{1}).has_value());
    REQUIRE_FALSE(db.table<async_item>().find(std::int64_t{2}).has_value());
}

TEST_CASE("sqlite client waits for an in-flight transaction before it is destroyed", "[async][client][sqlite]")
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = (std::filesystem::temp_directory_path() /
                       ("entityfy_transaction_async_destroy_" + std::to_string(stamp) + ".db"))
                          .string();
    std::filesystem::remove(path);

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool body_started = false;
    bool release_body = false;
    bool body_finished = false;

    {
        auto db = std::make_unique<entityfy::client>("sqlite://" + path);
        db->migrate<async_item>();

        auto pending = db->transaction_async([&](entityfy::client& tx) {
            {
                const std::lock_guard lock{gate_mutex};
                body_started = true;
            }
            gate_cv.notify_all();

            {
                std::unique_lock lock{gate_mutex};
                gate_cv.wait(lock, [&] { return release_body; });
            }

            tx.table<async_item>().insert(async_item{.id = 1, .name = "committed on destroy", .score = 1});

            {
                const std::lock_guard lock{gate_mutex};
                body_finished = true;
            }
        });

        {
            std::unique_lock lock{gate_mutex};
            gate_cv.wait(lock, [&] { return body_started; });
        }

        {
            const std::lock_guard lock{gate_mutex};
            release_body = true;
        }
        gate_cv.notify_all();

        // The worker is still inside the transaction when the client goes away;
        // its destructor drains and joins before the backend is released.
        db.reset();

        REQUIRE(body_finished);
        pending.get();
    }

    // The committed row survived the client that produced it.
    {
        entityfy::client reopened{"sqlite://" + path};
        REQUIRE(reopened.table<async_item>().count() == 1);
        REQUIRE(reopened.table<async_item>().find(std::int64_t{1})->name == "committed on destroy");
    }

    std::filesystem::remove(path);
}

#endif // ENTITYFY_ENABLE_SQLITE

#ifdef ENTITYFY_ENABLE_POSTGRESQL

TEST_CASE("client async mode follows the backend", "[async][client][postgresql]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};

    REQUIRE(db.async_mode() == entityfy::async_capability::native);
    REQUIRE(db.async_mode() != entityfy::async_capability::unsupported);
    REQUIRE(db.target_dialect() == entityfy::dialect::postgresql);
}

TEST_CASE("client async execute, batch and query match the sync path", "[async][client][postgresql]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"async_client_items\""});
    db.migrate<async_item>();

    const auto insert_sql = [](std::int64_t id, std::string name, std::int64_t score) {
        return entityfy::statement{
            .sql = "INSERT INTO \"async_client_items\" (\"id\", \"name\", \"score\") VALUES (?, ?, ?)",
            .binds = {entityfy::sql_value{id}, entityfy::sql_value{std::move(name)}, entityfy::sql_value{score}},
        };
    };

    const auto single = db.execute_async(insert_sql(1, "first", 10)).get();
    REQUIRE(single.rows_affected == 1);

    const auto batch = db.execute_batch_async(std::vector<entityfy::statement>{
        insert_sql(2, "second", 20),
        insert_sql(3, "third", 30),
    }).get();

    REQUIRE(batch.size() == 2);
    REQUIRE(batch[0].rows_affected == 1);
    REQUIRE(batch[1].rows_affected == 1);

    const auto rows = db.query_async(entityfy::statement{
        .sql = "SELECT \"name\", \"score\" FROM \"async_client_items\" ORDER BY \"score\""
    }).get();

    REQUIRE(rows.size() == 3);
    REQUIRE(rows[0].size() == 2);
    REQUIRE(entityfy::detail::value_to_string(rows[0][0]) == "first");
    REQUIRE(entityfy::detail::value_to_i64(rows[0][1]) == 10);
    REQUIRE(entityfy::detail::value_to_string(rows[1][0]) == "second");
    REQUIRE(entityfy::detail::value_to_i64(rows[1][1]) == 20);
    REQUIRE(entityfy::detail::value_to_string(rows[2][0]) == "third");
    REQUIRE(entityfy::detail::value_to_i64(rows[2][1]) == 30);

    // The synchronous path reads the same rows back and the connection is free
    // again once the async operations completed.
    const auto synchronous = db.query(entityfy::statement{
        .sql = "SELECT \"name\" FROM \"async_client_items\" ORDER BY \"score\""
    });
    REQUIRE(synchronous.size() == 3);
    REQUIRE(entityfy::detail::value_to_string(synchronous[2][0]) == "third");

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"async_client_items\""});
}

TEST_CASE("client transaction_async commits on its worker and returns the body value", "[async][client][postgresql]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"async_client_items\""});
    db.migrate<async_item>();

    std::thread::id body_thread;
    bool same_client = false;

    auto committed = db.transaction_async([&](entityfy::client& tx) {
        body_thread = std::this_thread::get_id();
        same_client = &tx == &db;

        const auto inserted = tx.table<async_item>().insert(async_item{.id = 1, .name = "committed", .score = 5});
        if(inserted.rows_affected != 1)
        {
            throw std::runtime_error{"transaction_async insert did not affect one row"};
        }

        return std::int64_t{21};
    });

    REQUIRE(committed.get() == 21);
    REQUIRE(body_thread != std::this_thread::get_id());
    // The body receives the same client, so it uses the same connection.
    REQUIRE(same_client);

    REQUIRE(db.table<async_item>().count() == 1);
    REQUIRE(db.table<async_item>().find(std::int64_t{1})->name == "committed");

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"async_client_items\""});
}

TEST_CASE("client transaction_async rolls back a failing body and rethrows", "[async][client][postgresql]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"async_client_items\""});
    db.migrate<async_item>();

    // A no-argument body: transaction_async mirrors transaction's deduction.
    auto failing = db.transaction_async([&]() -> std::int64_t {
        db.table<async_item>().insert(async_item{.id = 2, .name = "rolled back", .score = 6});
        throw std::runtime_error{"transaction_async body failed"};
    });

    REQUIRE_THROWS_WITH(failing.get(), "transaction_async body failed");
    REQUIRE(db.table<async_item>().count() == 0);
    REQUIRE_FALSE(db.table<async_item>().find(std::int64_t{2}).has_value());

    // The rollback finished before the task published the exception, so the
    // same client commits the next transaction.
    REQUIRE(db.transaction_async([&](entityfy::client& tx) {
        return tx.table<async_item>().insert(async_item{.id = 3, .name = "after rollback", .score = 7}).rows_affected;
    }).get() == 1);
    REQUIRE(db.table<async_item>().count() == 1);

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"async_client_items\""});
}

TEST_CASE("client transaction_async surfaces the backend error while the connection is busy", "[async][client][postgresql]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"async_client_items\""});
    db.migrate<async_item>();

    auto slow = db.query_async(entityfy::statement{.sql = "SELECT pg_sleep(0.2)"});
    auto rejected = db.transaction_async([&](entityfy::client& tx) {
        tx.table<async_item>().insert(async_item{.id = 4, .name = "rejected", .score = 8});
    });

    // The job's first statement is BEGIN, which surfaces the backend's own
    // single-flight logic_error unchanged.
    REQUIRE_THROWS_WITH(rejected.get(), "connection busy with an async operation");

    REQUIRE(slow.get().size() == 1);

    // The rejected job never entered the transaction, so the depth stayed zero.
    REQUIRE(db.transaction_async([&](entityfy::client& tx) {
        return tx.table<async_item>().insert(async_item{.id = 5, .name = "accepted", .score = 9}).rows_affected;
    }).get() == 1);
    REQUIRE(db.table<async_item>().count() == 1);
    REQUIRE_FALSE(db.table<async_item>().find(std::int64_t{4}).has_value());

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"async_client_items\""});
}

TEST_CASE("table async CRUD mirrors the sync results", "[async][table][postgresql]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"async_client_items\""});
    db.migrate<async_item>();

    auto items = db.table<async_item>();

    const std::int64_t alpha_id = 1;
    const std::int64_t beta_id = 2;
    const std::int64_t gamma_id = 3;

    const auto alpha = items.insert_async(async_item{.id = alpha_id, .name = "alpha", .score = 1}).get();
    REQUIRE(alpha.rows_affected == 1);

    const auto beta = items.create_async(async_item{.id = beta_id, .name = "beta", .score = 2}).get();
    REQUIRE(beta.rows_affected == 1);

    const auto gamma = items.insert_async(async_item{.id = gamma_id, .name = "gamma", .score = 3}).get();
    REQUIRE(gamma.rows_affected == 1);

    // find_async: key overloads and the templated id overload.
    const auto loaded = items.find_async(alpha_id).get();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->id == alpha_id);
    REQUIRE(loaded->name == "alpha");
    REQUIRE(loaded->score == 1);

    const auto by_key = items.find_async(item_key(beta_id)).get();
    REQUIRE(by_key.has_value());
    REQUIRE(by_key->name == "beta");

    REQUIRE(items.find_unique_async(gamma_id).get().has_value());
    REQUIRE_FALSE(items.find_async(std::int64_t{987654}).get().has_value());

    // find_one_async / find_many_async see the rows the async insert wrote.
    const auto one = items.find_one_async(entityfy::where(&async_item::name).eq(std::string{"beta"})).get();
    REQUIRE(one.has_value());
    REQUIRE(one->id == beta_id);

    const auto at_least_two = items.find_many_async(entityfy::where(&async_item::score).gte(std::int64_t{2})).get();
    REQUIRE(at_least_two.size() == 2);

    entityfy::query_options<async_item> options;
    options.filter = entityfy::where(&async_item::score).gte(std::int64_t{2});
    options.limit = 1;
    REQUIRE(items.find_many_async(options).get().size() == 1);

    const auto ordered = items.find_many_async(entityfy::query<async_item>().order_by(&async_item::score)).get();
    REQUIRE(ordered.size() == 3);
    REQUIRE(ordered[0].name == "alpha");
    REQUIRE(ordered[1].name == "beta");
    REQUIRE(ordered[2].name == "gamma");

    // The sync path reads what the async path wrote.
    REQUIRE(items.count() == 3);
    REQUIRE(items.count_async().get() == 3);
    REQUIRE(items.count_async(entityfy::where(&async_item::score).gte(std::int64_t{2})).get() == 2);

    REQUIRE(items.exists_async(entityfy::where(&async_item::name).eq(std::string{"gamma"})).get());
    REQUIRE_FALSE(items.exists_async(entityfy::where(&async_item::name).eq(std::string{"missing"})).get());

    entityfy::query_options<async_item> exists_options;
    exists_options.filter = entityfy::where(&async_item::name).eq(std::string{"alpha"});
    REQUIRE(items.exists_async(exists_options).get());

    REQUIRE(items.exists_async(entityfy::query<async_item>().where(entityfy::where(&async_item::score).eq(std::int64_t{3}))).get());

    // Chaining: the continuation runs on the completed task and sees the value.
    bool continuation_ran = false;
    const bool chained = items.find_async(alpha_id)
                             .then([&continuation_ran](std::optional<async_item> value) {
                                 continuation_ran = true;
                                 return value.has_value() && value->name == "alpha";
                             })
                             .get();
    REQUIRE(chained);
    REQUIRE(continuation_ran);

    const bool chained_missing = items.find_async(std::int64_t{987654})
                                     .then([](std::optional<async_item> value) { return value.has_value(); })
                                     .get();
    REQUIRE_FALSE(chained_missing);

    // update_async rewrites the whole row; the sync path observes it.
    auto probe = *loaded;
    probe.name = "alpha updated";
    REQUIRE(items.update_async(probe).get() == 1);
    REQUIRE(items.find(alpha_id)->name == "alpha updated");

    // update_partial_async writes only the listed columns.
    probe.score = 99;
    REQUIRE(items.update_partial_async(probe, &async_item::score).get() == 1);
    const auto after_partial = items.find(alpha_id);
    REQUIRE(after_partial->score == 99);
    REQUIRE(after_partial->name == "alpha updated");

    // remove_async deletes only the matching rows and keeps the empty-filter
    // guard of the sync path.
    REQUIRE_THROWS_AS(items.remove_async(entityfy::condition{}), std::invalid_argument);
    REQUIRE(items.remove_async(entityfy::where(&async_item::name).eq(std::string{"beta"})).get() == 1);
    REQUIRE_FALSE(items.exists(entityfy::where(&async_item::name).eq(std::string{"beta"})));
    REQUIRE(items.count_async().get() == 2);

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"async_client_items\""});
}

TEST_CASE("query_each_async feeds rows in order and stops early", "[async][client][postgresql]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"async_client_items\""});
    db.migrate<async_item>();

    auto items = db.table<async_item>();
    for(std::int64_t index = 0; index < 4; ++index)
    {
        const auto inserted =
            items.insert(async_item{.id = index + 1, .name = "row " + std::to_string(index + 1), .score = index + 1});
        REQUIRE(inserted.rows_affected == 1);
    }

    const auto stream_sql = entityfy::statement{
        .sql = "SELECT \"name\" FROM \"async_client_items\" ORDER BY \"score\""
    };

    // A false return stops the iteration: the fourth row is never delivered.
    std::vector<std::string> visited;
    std::vector<std::size_t> widths;
    db.query_each_async(stream_sql, [&](const entityfy::detail::row& current) {
        widths.push_back(current.size());
        visited.push_back(entityfy::detail::value_to_string(current.front()));
        return visited.size() < 3;
    }).get();

    REQUIRE(visited == std::vector<std::string>{"row 1", "row 2", "row 3"});
    // Every delivered row is complete: the consumer never sees a half-loaded one.
    REQUIRE(widths == std::vector<std::size_t>{1, 1, 1});

    std::vector<std::string> all;
    db.query_each_async(stream_sql, [&](const entityfy::detail::row& current) {
        all.push_back(entityfy::detail::value_to_string(current.front()));
        return true;
    }).get();

    REQUIRE(all == std::vector<std::string>{"row 1", "row 2", "row 3", "row 4"});

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"async_client_items\""});
}

#endif // ENTITYFY_ENABLE_POSTGRESQL
