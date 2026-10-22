// Honest comparison benchmark:
//   entityfy typed API  vs  raw SQLite (cached prepared)  vs  raw SQLite (prepare on every call)
// Same process, same schema, same rows, in-memory SQLite, single thread.
//
// Usage: entityfy_bench [row_count] [repetitions]

#include <entityfy/db_context.hpp>
#include <entityfy/entityfy.hpp>

#include <sqlite3.h>

#if defined(ENTITYFY_BENCH_POSTGRESQL)
#include <libpq-fe.h>
#endif


#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using clock_type = std::chrono::steady_clock;

struct [[= entityfy::table{"bench_users"}]] bench_user
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::unique, = entityfy::indexed, = entityfy::not_null, = entityfy::varchar{320}]]
    std::string email;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;
};

struct [[= entityfy::table{"bench_users_explicit"}]] bench_user_explicit
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::varchar{320}]]
    std::string email;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;
};

namespace
{

volatile std::uint64_t g_sink = 0;

struct timing
{
    double median_ns = 0.0;
    double min_ns = 0.0;
    double max_ns = 0.0;
};

double median_of(std::vector<double> values)
{
    if(values.empty())
    {
        return 0.0;
    }

    std::ranges::sort(values);
    const std::size_t middle = values.size() / 2;
    return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
}

template <typename Function>
timing measure(Function&& function, std::size_t iterations, std::size_t repetitions)
{
    const std::size_t warmup = std::min<std::size_t>(iterations / 10, 200);

    for(std::size_t index = 0; index < warmup; ++index)
    {
        function(index);
    }

    std::vector<double> samples;
    samples.reserve(repetitions);

    for(std::size_t repetition = 0; repetition < repetitions; ++repetition)
    {
        const auto started = clock_type::now();

        for(std::size_t index = 0; index < iterations; ++index)
        {
            function(index);
        }

        const auto finished = clock_type::now();
        samples.push_back(std::chrono::duration<double, std::nano>(finished - started).count() /
                          static_cast<double>(iterations));
    }

    timing result;
    result.median_ns = median_of(samples);
    result.min_ns = *std::ranges::min_element(samples);
    result.max_ns = *std::ranges::max_element(samples);
    return result;
}

// Context layer (change tracking) benchmark: the db_set/db_context paths that
// sit on top of the core timings measured below. Inserts, lookups and updates
// run against an in-memory database so they are comparable with the core
// numbers; only the cold (not yet tracked) lookup needs a file database and a
// second connection.
void run_context_bench(const std::vector<std::string>& emails,
                       const std::vector<std::string>& names,
                       std::size_t rows,
                       std::size_t repetitions)
{
    // ---- tracked insert (add + save_changes), median of 3 trials ----
    std::vector<double> insert_rates;

    for(int trial = 0; trial < 3; ++trial)
    {
        entityfy::db_context ctx{"sqlite://:memory:"};
        ctx.migrate<bench_user>();
        auto users = ctx.set<bench_user>();

        const auto started = clock_type::now();
        ctx.transaction([&] {
            for(std::size_t index = 0; index < rows; ++index)
            {
                users.add(bench_user{.email = emails[index], .name = names[index]});
            }
            ctx.save_changes();
        });
        const auto finished = clock_type::now();

        const double seconds = std::chrono::duration<double>(finished - started).count();
        insert_rates.push_back(static_cast<double>(rows) / seconds);
    }

    // ---- steady state: one loaded in-memory context ----
    double identity_ns = 0.0;
    double load_ns = 0.0;
    double update_ns = 0.0;

    {
        entityfy::db_context ctx{"sqlite://:memory:"};
        ctx.migrate<bench_user>();
        auto users = ctx.set<bench_user>();

        for(std::size_t index = 0; index < rows; ++index)
        {
            users.add(bench_user{.email = emails[index], .name = names[index]});
        }
        ctx.save_changes();

        const std::size_t lookups = std::min<std::size_t>(rows, 2000);

        const auto identity_hit = measure(
            [&](std::size_t index) {
                auto* user = users.find(static_cast<std::int64_t>(index % lookups) + 1);
                g_sink += user == nullptr ? 0u : static_cast<std::uint64_t>(user->id);
            },
            lookups,
            repetitions
        );
        identity_ns = identity_hit.median_ns;

        // Dirty-column update: one changed field, one transaction per call.
        auto* first = users.find(1);
        if(first == nullptr)
        {
            throw std::runtime_error{"context benchmark lost the first row"};
        }

        std::vector<double> update_samples;
        update_samples.reserve(repetitions);

        for(std::size_t repetition = 0; repetition < repetitions; ++repetition)
        {
            first->name = "Updated " + std::to_string(repetition);

            const auto started = clock_type::now();
            const auto result = ctx.save_changes();
            const auto finished = clock_type::now();

            update_samples.push_back(std::chrono::duration<double, std::nano>(finished - started).count());
            g_sink += result.updated;
        }

        update_ns = median_of(update_samples);
    }

    // ---- cold lookup: fresh context, every key touched once (file database) ----
    const char* path = "bench_context.db";
    std::filesystem::remove(path);

    double cold_ns = 0.0;
    const std::size_t cold_rows = std::min<std::size_t>(rows, 5000);

    {
        entityfy::db_context seed{"sqlite://bench_context.db"};
        seed.migrate<bench_user>();
        auto seed_users = seed.set<bench_user>();

        seed.transaction([&] {
            for(std::size_t index = 0; index < cold_rows; ++index)
            {
                seed_users.add(bench_user{.email = emails[index], .name = names[index]});
            }
            seed.save_changes();
        });
    }

    {
        entityfy::db_context cold{"sqlite://bench_context.db"};
        auto cold_users = cold.set<bench_user>();

        const auto cold_started = clock_type::now();
        for(std::size_t index = 0; index < cold_rows; ++index)
        {
            auto* user = cold_users.find(static_cast<std::int64_t>(index) + 1);
            g_sink += user == nullptr ? 0u : static_cast<std::uint64_t>(user->id);
        }
        const auto cold_finished = clock_type::now();

        cold_ns = std::chrono::duration<double, std::nano>(cold_finished - cold_started).count() /
                  static_cast<double>(cold_rows);
    }

    // Bulk load through the tracker from a context that tracks nothing yet,
    // so every row really is materialized and attached (not a hash hit).
    {
        entityfy::db_context loader{"sqlite://bench_context.db"};
        auto loader_users = loader.set<bench_user>();

        const auto load_started = clock_type::now();
        const auto all_rows = loader_users.to_list();
        const auto load_finished = clock_type::now();
        load_ns = std::chrono::duration<double, std::nano>(load_finished - load_started).count() /
                  static_cast<double>(all_rows.size());
        g_sink += all_rows.size();
    }

    std::filesystem::remove(path);

    std::cout << "\n=== context layer benchmark (change tracking) ===\n";
    std::cout << std::format("SQLite {} | rows={} | repetitions={} | single thread\n\n",
                             sqlite3_libversion(),
                             rows,
                             repetitions);
    std::cout << std::format("{:<34}{:>16.0f} rows/s\n", "add + save_changes", median_of(insert_rates));
    std::cout << std::format("{:<34}{:>16.0f} ns\n", "find (identity map hit)", identity_ns);
    std::cout << std::format("{:<34}{:>16.0f} ns\n", "find (database hit, cold)", cold_ns);
    std::cout << std::format("{:<34}{:>16.0f} ns/row\n", "to_list (load + track)", load_ns);
    std::cout << std::format("{:<34}{:>16.0f} ns\n", "save_changes (1 dirty row)", update_ns);

    if(load_ns > 0.0)
    {
        std::cout << std::format("\ncold find / to_list per row: {:.0f}x\n", cold_ns / load_ns);
    }

    std::cout << "\nNote: cold find and to_list use a file backed database and a fresh\n"
                 "      context (real SELECT + materialize + attach); the other context\n"
                 "      numbers are in-memory, like the core table above.\n";
}

void check_sqlite(int code, sqlite3* database, std::string_view what)
{
    if(code != SQLITE_OK && code != SQLITE_ROW && code != SQLITE_DONE)
    {
        throw std::runtime_error{std::string{what} + ": " + sqlite3_errmsg(database)};
    }
}

class database
{
public:
    explicit database(std::string_view path)
    {
        if(sqlite3_open(std::string{path}.c_str(), &handle_) != SQLITE_OK)
        {
            throw std::runtime_error{std::string{"sqlite3_open: "} + sqlite3_errmsg(handle_)};
        }
    }

    ~database()
    {
        if(handle_ != nullptr)
        {
            sqlite3_close(handle_);
        }
    }

    database(const database&) = delete;
    database& operator=(const database&) = delete;

    [[nodiscard]] sqlite3* get() const noexcept
    {
        return handle_;
    }

    void execute(std::string_view sql)
    {
        char* error = nullptr;
        const int code = sqlite3_exec(handle_, std::string{sql}.c_str(), nullptr, nullptr, &error);

        if(code != SQLITE_OK)
        {
            const std::string message = error != nullptr ? error : sqlite3_errmsg(handle_);
            sqlite3_free(error);
            throw std::runtime_error{"sqlite3_exec: " + message};
        }
    }

private:
    sqlite3* handle_ = nullptr;
};

class statement
{
public:
    statement(sqlite3* database_handle, std::string_view sql)
    {
        check_sqlite(sqlite3_prepare_v2(database_handle, std::string{sql}.c_str(), -1, &handle_, nullptr),
                     database_handle,
                     "sqlite3_prepare_v2");
    }

    ~statement()
    {
        if(handle_ != nullptr)
        {
            sqlite3_finalize(handle_);
        }
    }

    statement(const statement&) = delete;
    statement& operator=(const statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept
    {
        return handle_;
    }

    void reset()
    {
        sqlite3_reset(handle_);
        sqlite3_clear_bindings(handle_);
    }

private:
    sqlite3_stmt* handle_ = nullptr;
};

struct raw_user
{
    std::int64_t id = 0;
    std::string  email;
    std::string  name;
};

raw_user read_user(sqlite3_stmt* query)
{
    raw_user user;
    user.id = sqlite3_column_int64(query, 0);

    const auto* email = sqlite3_column_text(query, 1);
    const auto* name = sqlite3_column_text(query, 2);
    user.email = email != nullptr ? reinterpret_cast<const char*>(email) : "";
    user.name = name != nullptr ? reinterpret_cast<const char*>(name) : "";
    return user;
}

std::optional<raw_user> raw_find(std::int64_t id, statement& query)
{
    query.reset();
    sqlite3_bind_int64(query.get(), 1, id);

    if(sqlite3_step(query.get()) != SQLITE_ROW)
    {
        query.reset();
        return std::nullopt;
    }

    auto user = read_user(query.get());
    query.reset();
    return user;
}

std::optional<raw_user> raw_find_by_email(std::string_view email, statement& query)
{
    query.reset();
    sqlite3_bind_text(query.get(), 1, email.data(), static_cast<int>(email.size()), SQLITE_TRANSIENT);

    if(sqlite3_step(query.get()) != SQLITE_ROW)
    {
        query.reset();
        return std::nullopt;
    }

    auto user = read_user(query.get());
    query.reset();
    return user;
}

bool raw_exists(std::string_view email, statement& query)
{
    query.reset();
    sqlite3_bind_text(query.get(), 1, email.data(), static_cast<int>(email.size()), SQLITE_TRANSIENT);
    const bool found = sqlite3_step(query.get()) == SQLITE_ROW;
    query.reset();
    return found;
}

std::int64_t raw_count(statement& query)
{
    query.reset();

    const bool has_row = sqlite3_step(query.get()) == SQLITE_ROW;
    const std::int64_t count = has_row ? sqlite3_column_int64(query.get(), 0) : 0;
    query.reset();
    return count;
}

std::size_t raw_range(std::string_view prefix, statement& query)
{
    query.reset();
    sqlite3_bind_text(query.get(), 1, prefix.data(), static_cast<int>(prefix.size()), SQLITE_TRANSIENT);

    std::size_t rows = 0;
    while(sqlite3_step(query.get()) == SQLITE_ROW)
    {
        // entityfy find_many materializes 20 full users; the baseline must also
        // read id + email + name and copy the strings.
        (void)sqlite3_column_int64(query.get(), 0);

        const auto* email = sqlite3_column_text(query.get(), 1);
        const auto* name = sqlite3_column_text(query.get(), 2);
        const std::string email_text = email != nullptr ? reinterpret_cast<const char*>(email) : "";
        const std::string name_text = name != nullptr ? reinterpret_cast<const char*>(name) : "";
        g_sink += email_text.size() + name_text.size();
        ++rows;
    }

    query.reset();
    return rows;
}

std::size_t raw_update_read_write(std::int64_t id, statement& select_query, statement& update_query)
{
    const auto user = raw_find(id, select_query);
    if(!user)
    {
        return 0;
    }

    // entityfy table.update writes every non-PK column; the baseline must write
    // email + name too (writing only name inflated the gap to 8x).
    update_query.reset();
    sqlite3_bind_text(update_query.get(), 1, user->email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update_query.get(), 2, "Updated", -1, SQLITE_STATIC);
    sqlite3_bind_int64(update_query.get(), 3, id);
    const int code = sqlite3_step(update_query.get());
    update_query.reset();

    if(code != SQLITE_DONE)
    {
        throw std::runtime_error{"raw update failed"};
    }

    return static_cast<std::size_t>(sqlite3_changes(sqlite3_db_handle(select_query.get())));
}

std::size_t raw_delete(std::int64_t id, statement& query)
{
    query.reset();
    sqlite3_bind_int64(query.get(), 1, id);
    const int code = sqlite3_step(query.get());
    query.reset();

    if(code != SQLITE_DONE)
    {
        throw std::runtime_error{"raw delete failed"};
    }

    return static_cast<std::size_t>(sqlite3_changes(sqlite3_db_handle(query.get())));
}

void print_timing_row(
    std::string_view name,
    const timing& entityfy_timing,
    const timing& raw_cached,
    const timing& raw_prepared_each,
    bool include_prepare_each)
{
    const double entityfy_vs_cached = raw_cached.median_ns > 0.0 ? entityfy_timing.median_ns / raw_cached.median_ns : 0.0;

    if(include_prepare_each)
    {
        const double entityfy_vs_prepare =
            raw_prepared_each.median_ns > 0.0 ? entityfy_timing.median_ns / raw_prepared_each.median_ns : 0.0;

        std::cout << std::format("{:<26}{:>12.0f}{:>14.0f}{:>18.0f}{:>11.2f}{:>14.2f}\n",
                                 name,
                                 entityfy_timing.median_ns,
                                 raw_cached.median_ns,
                                 raw_prepared_each.median_ns,
                                 entityfy_vs_cached,
                                 entityfy_vs_prepare);
    }
    else
    {
        std::cout << std::format("{:<26}{:>12.0f}{:>14.0f}{:>18}{:>11.2f}{:>14}\n",
                                 name,
                                 entityfy_timing.median_ns,
                                 raw_cached.median_ns,
                                 "-",
                                 entityfy_vs_cached,
                                 "-");
    }
}

struct insert_trial
{
    double rows_per_second = 0.0;
    std::int64_t row_id = 0;
};

insert_trial insert_with_entityfy(std::size_t rows, const std::vector<std::string>& emails,
                                 const std::vector<std::string>& names)
{
    entityfy::client database{"sqlite://:memory:"};
    auto users = database.table<bench_user>();
    users.migrate();

    insert_trial trial;

    const auto started = clock_type::now();
    database.transaction([&] {
        for(std::size_t index = 0; index < rows; ++index)
        {
            bench_user user;
            user.email = emails[index];
            user.name = names[index];
            auto result = users.insert(user);
            trial.row_id = result.last_insert_id;
        }
    });
    const auto finished = clock_type::now();

    const double seconds = std::chrono::duration<double>(finished - started).count();
    trial.rows_per_second = static_cast<double>(rows) / seconds;
    return trial;
}

insert_trial insert_raw(std::size_t rows, const std::vector<std::string>& emails,
                        const std::vector<std::string>& names)
{
    database raw{":memory:"};
    raw.execute("CREATE TABLE bench_users (id INTEGER PRIMARY KEY AUTOINCREMENT, email TEXT NOT NULL, name TEXT NOT NULL)");
    raw.execute("CREATE UNIQUE INDEX uidx_bench_users_email ON bench_users(email)");

    statement insert_query{raw.get(), "INSERT INTO bench_users (email, name) VALUES (?, ?)"};

    insert_trial trial;

    raw.execute("BEGIN");
    const auto started = clock_type::now();

    for(std::size_t index = 0; index < rows; ++index)
    {
        insert_query.reset();
        sqlite3_bind_text(insert_query.get(), 1, emails[index].c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_query.get(), 2, names[index].c_str(), -1, SQLITE_TRANSIENT);

        if(sqlite3_step(insert_query.get()) != SQLITE_DONE)
        {
            throw std::runtime_error{"raw insert failed"};
        }

        trial.row_id = sqlite3_last_insert_rowid(raw.get());
    }

    const auto finished = clock_type::now();
    raw.execute("COMMIT");

    const double seconds = std::chrono::duration<double>(finished - started).count();
    trial.rows_per_second = static_cast<double>(rows) / seconds;
    return trial;
}


#if defined(ENTITYFY_BENCH_POSTGRESQL)

namespace
{

void pg_check(PGconn* connection, PGresult* result, std::string_view what)
{
    if(result == nullptr)
    {
        throw std::runtime_error{std::string{what} + ": " + PQerrorMessage(connection)};
    }

    const auto status = PQresultStatus(result);
    if(status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
    {
        const std::string message = PQresultErrorMessage(result);
        PQclear(result);
        throw std::runtime_error{std::string{what} + ": " + message};
    }

    PQclear(result);
}

void run_postgres_bench(const std::string& uri, std::size_t rows, std::size_t repetitions)
{
    PGconn* connection = PQconnectdb(uri.c_str());
    if(connection == nullptr || PQstatus(connection) != CONNECTION_OK)
    {
        const std::string message = connection != nullptr ? PQerrorMessage(connection) : "no connection";
        if(connection != nullptr)
        {
            PQfinish(connection);
        }

        std::cout << "postgres benchmark skipped: " << message << "\n";
        return;
    }

    pg_check(connection,
             PQexec(connection, "DROP TABLE IF EXISTS \"bench_users_explicit\""),
             "drop");
    pg_check(connection,
             PQexec(connection,
                    "CREATE TABLE \"bench_users_explicit\" ("
                    "\"id\" BIGINT PRIMARY KEY, "
                    "\"email\" VARCHAR(320) NOT NULL, "
                    "\"name\" VARCHAR(120) NOT NULL)"),
             "create");
    pg_check(connection,
             PQexec(connection,
                    "CREATE UNIQUE INDEX \"uidx_bench_users_explicit_email\" "
                    "ON \"bench_users_explicit\" (\"email\")"),
             "index");

    std::vector<std::string> emails;
    std::vector<std::string> names;
    emails.reserve(rows);
    names.reserve(rows);

    for(std::size_t index = 0; index < rows; ++index)
    {
        emails.push_back("user" + std::to_string(index) + "@bench.test");
        names.push_back("User " + std::to_string(index));
    }

    std::cout << std::format("\n=== PostgreSQL benchmark ({} rows) ===\n", rows);

    // entityfy path
    entityfy::client database{uri};
    auto            users = database.table<bench_user_explicit>();

    std::vector<bench_user_explicit> models;
    models.reserve(rows);
    for(std::size_t index = 0; index < rows; ++index)
    {
        models.push_back(bench_user_explicit{
            .id = static_cast<std::int64_t>(index + 1),
            .email = emails[index],
            .name = names[index]});
    }

    const auto insert_started = clock_type::now();
    const auto insert_result = users.insert_many(models);
    const auto insert_finished = clock_type::now();

    const double insert_seconds = std::chrono::duration<double>(insert_finished - insert_started).count();

    std::cout << std::format("INSERT (explicit key, single statement)\n");
    std::cout << std::format("{:<26}{:>16.0f} rows/s\n",
                             "entityfy insert_many",
                             static_cast<double>(insert_result.rows_affected) / insert_seconds);

    const std::string pk_sql = "SELECT \"id\", \"email\", \"name\" FROM \"bench_users_explicit\" WHERE \"id\" = $1";
    const std::string email_sql =
        "SELECT \"id\", \"email\", \"name\" FROM \"bench_users_explicit\" WHERE \"email\" = $1";

    pg_check(connection, PQprepare(connection, "bench_pk", pk_sql.c_str(), 0, nullptr), "prepare pk");
    pg_check(connection, PQprepare(connection, "bench_email", email_sql.c_str(), 0, nullptr), "prepare email");

    const auto entityfy_pk = measure(
        [&](std::size_t index) {
            const auto found = users.find(static_cast<std::int64_t>(index + 1));
            g_sink += found ? static_cast<std::uint64_t>(found->id) : 0;
        },
        std::min<std::size_t>(rows, 200),
        repetitions);

    const auto raw_prepared_pk = measure(
        [&](std::size_t index) {
            const std::string value = std::to_string(index + 1);
            const char*        values[1] = {value.c_str()};
            PGresult*         result = PQexecPrepared(connection, "bench_pk", 1, values, nullptr, nullptr, 0);
            if(result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK)
            {
                g_sink += static_cast<std::uint64_t>(PQntuples(result));
            }

            if(result != nullptr)
            {
                PQclear(result);
            }
        },
        std::min<std::size_t>(rows, 200),
        repetitions);

    const auto raw_params_pk = measure(
        [&](std::size_t index) {
            const std::string value = std::to_string(index + 1);
            const char*        values[1] = {value.c_str()};
            PGresult*         result =
                PQexecParams(connection, pk_sql.c_str(), 1, nullptr, values, nullptr, nullptr, 0);
            if(result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK)
            {
                g_sink += static_cast<std::uint64_t>(PQntuples(result));
            }

            if(result != nullptr)
            {
                PQclear(result);
            }
        },
        std::min<std::size_t>(rows, 200),
        repetitions);

    const auto entityfy_email = measure(
        [&](std::size_t index) {
            const auto found = users.find_one(entityfy::where(&bench_user_explicit::email).eq(emails[index]));
            g_sink += found ? static_cast<std::uint64_t>(found->id) : 0;
        },
        std::min<std::size_t>(rows, 200),
        repetitions);

    const auto raw_prepared_email = measure(
        [&](std::size_t index) {
            const char* values[1] = {emails[index].c_str()};
            PGresult*  result = PQexecPrepared(connection, "bench_email", 1, values, nullptr, nullptr, 0);
            if(result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK)
            {
                g_sink += static_cast<std::uint64_t>(PQntuples(result));
            }

            if(result != nullptr)
            {
                PQclear(result);
            }
        },
        std::min<std::size_t>(rows, 200),
        repetitions);

    std::cout << std::format(
        "\n{:<26}{:>14}{:>16}{:>16}{:>11}{:>14}\n",
        "operation",
        "entityfy us",
        "raw prepared",
        "raw params",
        "E/prep",
        "E/params");
    std::cout << std::string(95, '-') << "\n";

    const auto row = [&](std::string_view name,
                         const timing& entityfy_timing,
                         const timing& prepared,
                         const timing& params) {
        const double vs_prepared =
            prepared.median_ns > 0.0 ? entityfy_timing.median_ns / prepared.median_ns : 0.0;
        const double vs_params = params.median_ns > 0.0 ? entityfy_timing.median_ns / params.median_ns : 0.0;

        std::cout << std::format("{:<26}{:>14.1f}{:>16.1f}{:>16.1f}{:>11.2f}{:>14.2f}\n",
                                 name,
                                 entityfy_timing.median_ns / 1000.0,
                                 prepared.median_ns / 1000.0,
                                 params.median_ns / 1000.0,
                                 vs_prepared,
                                 vs_params);
    };

    row("find (primary key)", entityfy_pk, raw_prepared_pk, raw_params_pk);
    row("find_one (unique email)", entityfy_email, raw_prepared_email, raw_params_pk);

    PQfinish(connection);
    std::cout << "\nNote: PostgreSQL timings include network latency; the server runs on the same machine.\n";
}

} // namespace

#endif

} // namespace

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;

    std::size_t rows = 20000;
    std::size_t repetitions = 7;

    if(argc > 1)
    {
        rows = std::stoull(argv[1]);
    }

    if(argc > 2)
    {
        repetitions = std::stoull(argv[2]);
    }

    if(rows < 100 || repetitions == 0 || repetitions > rows / 2)
    {
        std::cerr << "for a fair delete measurement: rows >= 100 and 0 < repetitions <= rows/2\n";
        return 2;
    }

    std::vector<std::string> emails;
    std::vector<std::string> names;
    emails.reserve(rows);
    names.reserve(rows);

    for(std::size_t index = 0; index < rows; ++index)
    {
        emails.push_back("user" + std::to_string(index) + "@bench.test");
        names.push_back("User " + std::to_string(index));
    }

#if defined(ENTITYFY_BENCH_POSTGRESQL)
    if(const char* pg_uri = std::getenv("ENTITYFY_PG_URI"); pg_uri != nullptr && *pg_uri != '\0')
    {
        run_postgres_bench(pg_uri, rows, repetitions);
    }
#endif

    std::cout << std::format("=== entityfy query benchmark ===\n");
    std::cout << std::format("SQLite {} | rows={} | repetitions={} | in-memory | single thread\n\n",
                             sqlite3_libversion(),
                             rows,
                             repetitions);

    // ---- insert throughput (3 trials, median) ----
    std::vector<double> entityfy_insert_rates;
    std::vector<double> raw_insert_rates;

    for(int trial = 0; trial < 3; ++trial)
    {
        entityfy_insert_rates.push_back(insert_with_entityfy(rows, emails, names).rows_per_second);
        raw_insert_rates.push_back(insert_raw(rows, emails, names).rows_per_second);
    }

    const double entityfy_insert = median_of(entityfy_insert_rates);
    const double raw_insert = median_of(raw_insert_rates);

    std::cout << std::format("INSERT (single transaction, median of 3 trials)\n");
    std::cout << std::format("{:<26}{:>16.0f} rows/s\n", "entityfy typed insert", entityfy_insert);
    std::cout << std::format("{:<26}{:>16.0f} rows/s\n", "raw prepared insert", raw_insert);
    std::cout << std::format("{:<26}{:>16.2f}x\n\n", "entityfy / raw", raw_insert / entityfy_insert);

    // Multi-row INSERT: a single statement for the explicit-key model, compared against the row-by-row path.
    {
        entityfy::client explicit_db{"sqlite://:memory:"};
        auto            explicit_users = explicit_db.table<bench_user_explicit>();
        explicit_users.migrate();

        std::vector<bench_user_explicit> explicit_models;
        explicit_models.reserve(rows);
        for(std::size_t index = 0; index < rows; ++index)
        {
            explicit_models.push_back(bench_user_explicit{
                .id = static_cast<std::int64_t>(index + 1),
                .email = emails[index],
                .name = names[index]});
        }

        const auto batch_started = clock_type::now();
        const auto batch_result = explicit_users.insert_many(explicit_models);
        const auto batch_finished = clock_type::now();

        std::vector<bench_user_explicit> row_models;
        row_models.reserve(rows);
        for(std::size_t index = 0; index < rows; ++index)
        {
            row_models.push_back(bench_user_explicit{
                .id = static_cast<std::int64_t>(rows + index + 1),
                .email = "row" + emails[index],
                .name = names[index]});
        }

        const auto row_started = clock_type::now();
        for(const auto& model: row_models)
        {
            const auto inserted = explicit_users.insert(model);
            g_sink += inserted.rows_affected;
        }
        const auto row_finished = clock_type::now();

        const double batch_seconds = std::chrono::duration<double>(batch_finished - batch_started).count();
        const double row_seconds = std::chrono::duration<double>(row_finished - row_started).count();

        std::cout << std::format("INSERT (explicit key, {} rows)\n", rows);
        std::cout << std::format("{:<26}{:>16.0f} rows/s\n", "entityfy insert_many",
                                 static_cast<double>(batch_result.rows_affected) / batch_seconds);
        std::cout << std::format("{:<26}{:>16.0f} rows/s\n", "entityfy insert (row by row)",
                                 static_cast<double>(rows) / row_seconds);
        std::cout << std::format("{:<26}{:>16.2f}x\n\n", "insert_many / row by row",
                                 row_seconds / batch_seconds);
    }

    // ---- persistent databases for query comparison ----
    entityfy::client entityfy_db{"sqlite://:memory:"};
    auto entityfy_users = entityfy_db.table<bench_user>();
    entityfy_users.migrate();

    std::vector<std::int64_t> entityfy_ids;
    entityfy_ids.reserve(rows);

    entityfy_db.transaction([&] {
        for(std::size_t index = 0; index < rows; ++index)
        {
            bench_user user;
            user.email = emails[index];
            user.name = names[index];
            const auto result = entityfy_users.insert(user);
            entityfy_ids.push_back(result.last_insert_id);
        }
    });

    database raw_db{":memory:"};
    raw_db.execute("CREATE TABLE bench_users (id INTEGER PRIMARY KEY AUTOINCREMENT, email TEXT NOT NULL, name TEXT NOT NULL)");
    raw_db.execute("CREATE UNIQUE INDEX uidx_bench_users_email ON bench_users(email)");

    std::vector<std::int64_t> raw_ids;
    raw_ids.reserve(rows);

    {
        statement insert_query{raw_db.get(), "INSERT INTO bench_users (email, name) VALUES (?, ?)"};
        raw_db.execute("BEGIN");

        for(std::size_t index = 0; index < rows; ++index)
        {
            insert_query.reset();
            sqlite3_bind_text(insert_query.get(), 1, emails[index].c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_query.get(), 2, names[index].c_str(), -1, SQLITE_TRANSIENT);

            if(sqlite3_step(insert_query.get()) != SQLITE_DONE)
            {
                throw std::runtime_error{"raw seed insert failed"};
            }

            raw_ids.push_back(sqlite3_last_insert_rowid(raw_db.get()));
        }

        raw_db.execute("COMMIT");
    }

    // ---- correctness before timing ----
    if(entityfy_users.count() != rows)
    {
        std::cerr << "FAIL: entityfy count mismatch\n";
        return 1;
    }

    statement raw_pk_query{raw_db.get(), "SELECT id, email, name FROM bench_users WHERE id = ? LIMIT 1"};
    statement raw_email_query{raw_db.get(), "SELECT id, email, name FROM bench_users WHERE email = ? LIMIT 1"};

    // persistent statement for raw_count
    statement raw_count_query{raw_db.get(), "SELECT COUNT(*) FROM bench_users"};
    if(raw_count(raw_count_query) != static_cast<std::int64_t>(rows))
    {
        std::cerr << "FAIL: raw count mismatch\n";
        return 1;
    }

    if(raw_ids.size() != entityfy_ids.size() || raw_ids.front() != entityfy_ids.front() ||
       raw_ids.back() != entityfy_ids.back())
    {
        std::cerr << "FAIL: id sequences differ\n";
        return 1;
    }

    const auto entityfy_sample = entityfy_users.find(entityfy_ids.front());
    const auto raw_sample = raw_find(raw_ids.front(), raw_pk_query);

    if(!entityfy_sample || !raw_sample || entityfy_sample->email != raw_sample->email ||
       entityfy_sample->name != raw_sample->name)
    {
        std::cerr << "FAIL: sample lookup mismatch\n";
        return 1;
    }

    const std::string sample_email = entityfy_sample->email;
    const auto entityfy_by_email = entityfy_users.find_one(entityfy::where(&bench_user::email).eq(sample_email));
    const auto raw_by_email = raw_find_by_email(sample_email, raw_email_query);

    if(!entityfy_by_email || !raw_by_email || entityfy_by_email->id != raw_by_email->id)
    {
        std::cerr << "FAIL: unique lookup mismatch\n";
        return 1;
    }

    std::cout << "[ok] insert and basic reads verified\n\n";

    // ---- query benchmark ----
    std::vector<std::size_t> order(rows);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 generator{1234567};
    std::shuffle(order.begin(), order.end(), generator);

    const std::size_t point_iterations = std::min<std::size_t>(rows, 2000);
    const std::size_t scan_iterations = std::min<std::size_t>(rows, 200);

    statement raw_exists_query{raw_db.get(), "SELECT 1 FROM bench_users WHERE email = ? LIMIT 1"};
    statement raw_range_query{
        raw_db.get(),
        "SELECT id, email, name FROM bench_users WHERE name LIKE ? ESCAPE '\\' ORDER BY id ASC LIMIT 20"};
    statement raw_update_select{raw_db.get(), "SELECT id, email, name FROM bench_users WHERE id = ? LIMIT 1"};
    statement raw_update_query{raw_db.get(), "UPDATE bench_users SET email = ?, name = ? WHERE id = ?"};
    statement raw_delete_query{raw_db.get(), "DELETE FROM bench_users WHERE id = ?"};

    std::cout << std::format(
        "{:<26}{:>12}{:>14}{:>18}{:>11}{:>14}\n",
        "operation",
        "entityfy ns",
        "raw cached",
        "raw prep-each",
        "R/cache",
        "R/prep");
    std::cout << std::string(95, '-') << "\n";

    const auto entityfy_pk = measure(
        [&](std::size_t index) {
            const auto user = entityfy_users.find(entityfy_ids[order[index % point_iterations]]);
            g_sink += user ? static_cast<std::uint64_t>(user->id) : 0;
        },
        point_iterations,
        repetitions);

    const auto raw_pk_cached = measure(
        [&](std::size_t index) {
            const auto user = raw_find(raw_ids[order[index % point_iterations]], raw_pk_query);
            g_sink += user ? static_cast<std::uint64_t>(user->id) : 0;
        },
        point_iterations,
        repetitions);

    const auto raw_pk_prepared = measure(
        [&](std::size_t index) {
            statement query{raw_db.get(), "SELECT id, email, name FROM bench_users WHERE id = ? LIMIT 1"};
            const auto user = raw_find(raw_ids[order[index % point_iterations]], query);
            g_sink += user ? static_cast<std::uint64_t>(user->id) : 0;
        },
        point_iterations,
        repetitions);

    print_timing_row("find (primary key)", entityfy_pk, raw_pk_cached, raw_pk_prepared, true);

    const auto entityfy_unique = measure(
        [&](std::size_t index) {
            const auto& email = emails[order[index % point_iterations]];
            const auto user = entityfy_users.find_one(entityfy::where(&bench_user::email).eq(email));
            g_sink += user ? static_cast<std::uint64_t>(user->id) : 0;
        },
        point_iterations,
        repetitions);

    const auto raw_unique_cached = measure(
        [&](std::size_t index) {
            const auto& email = emails[order[index % point_iterations]];
            const auto user = raw_find_by_email(email, raw_email_query);
            g_sink += user ? static_cast<std::uint64_t>(user->id) : 0;
        },
        point_iterations,
        repetitions);

    const auto raw_unique_prepared = measure(
        [&](std::size_t index) {
            statement query{raw_db.get(), "SELECT id, email, name FROM bench_users WHERE email = ? LIMIT 1"};
            const auto& email = emails[order[index % point_iterations]];
            const auto user = raw_find_by_email(email, query);
            g_sink += user ? static_cast<std::uint64_t>(user->id) : 0;
        },
        point_iterations,
        repetitions);

    print_timing_row("find_one (unique email)", entityfy_unique, raw_unique_cached, raw_unique_prepared, true);

    const auto entityfy_exists = measure(
        [&](std::size_t index) {
            const auto& email = emails[order[index % point_iterations]];
            g_sink += entityfy_users.exists(entityfy::where(&bench_user::email).eq(email)) ? 1 : 0;
        },
        point_iterations,
        repetitions);

    const auto raw_exists_cached = measure(
        [&](std::size_t index) {
            const auto& email = emails[order[index % point_iterations]];
            g_sink += raw_exists(email, raw_exists_query) ? 1 : 0;
        },
        point_iterations,
        repetitions);

    const auto raw_exists_prepared = measure(
        [&](std::size_t index) {
            statement query{raw_db.get(), "SELECT 1 FROM bench_users WHERE email = ? LIMIT 1"};
            const auto& email = emails[order[index % point_iterations]];
            g_sink += raw_exists(email, query) ? 1 : 0;
        },
        point_iterations,
        repetitions);

    print_timing_row("exists (unique email)", entityfy_exists, raw_exists_cached, raw_exists_prepared, true);

    const auto entityfy_range = measure(
        [&](std::size_t) {
            const auto result = entityfy_users.find_many(
                entityfy::query<bench_user>(entityfy::where(&bench_user::name).starts_with("User 1"))
                    .order_by(&bench_user::id)
                    .take(20));
            g_sink += result.size();
        },
        scan_iterations,
        repetitions);

    const auto raw_range_cached = measure(
        [&](std::size_t) {
            g_sink += raw_range("User 1%", raw_range_query);
        },
        scan_iterations,
        repetitions);

    const auto raw_range_prepared = measure(
        [&](std::size_t) {
            statement query{
                raw_db.get(),
                "SELECT id, email, name FROM bench_users WHERE name LIKE ? ESCAPE '\\' ORDER BY id ASC LIMIT 20"};
            g_sink += raw_range("User 1%", query);
        },
        scan_iterations,
        repetitions);

    print_timing_row("find_many (LIKE, LIMIT 20)", entityfy_range, raw_range_cached, raw_range_prepared, true);

    const auto entityfy_count = measure(
        [&](std::size_t) {
            g_sink += entityfy_users.count();
        },
        scan_iterations,
        repetitions);

    const auto raw_count_cached = measure(
        [&](std::size_t) {
            g_sink += static_cast<std::uint64_t>(raw_count(raw_count_query));
        },
        scan_iterations,
        repetitions);

    const auto raw_count_prepared = measure(
        [&](std::size_t) {
            statement query{raw_db.get(), "SELECT COUNT(*) FROM bench_users"};
            g_sink += static_cast<std::uint64_t>(raw_count(query));
        },
        scan_iterations,
        repetitions);

    print_timing_row("count", entityfy_count, raw_count_cached, raw_count_prepared, true);

    const std::size_t update_iterations = std::min<std::size_t>(rows, 200);

    const auto entityfy_update = measure(
        [&](std::size_t index) {
            const auto id = entityfy_ids[order[index % update_iterations]];
            auto user = entityfy_users.find(id);
            if(user)
            {
                user->name = "Updated";
                g_sink += entityfy_users.update(*user);
            }
        },
        update_iterations,
        repetitions);

    const auto raw_update_cached = measure(
        [&](std::size_t index) {
            const auto id = raw_ids[order[index % update_iterations]];
            g_sink += raw_update_read_write(id, raw_update_select, raw_update_query);
        },
        update_iterations,
        repetitions);

    const auto raw_update_prepared = measure(
        [&](std::size_t index) {
            statement select_query{raw_db.get(), "SELECT id, email, name FROM bench_users WHERE id = ? LIMIT 1"};
            statement update_query{raw_db.get(), "UPDATE bench_users SET email = ?, name = ? WHERE id = ?"};
            const auto id = raw_ids[order[index % update_iterations]];
            g_sink += raw_update_read_write(id, select_query, update_query);
        },
        update_iterations,
        repetitions);

    print_timing_row("update (read + write)", entityfy_update, raw_update_cached, raw_update_prepared, true);

    // Every measurement call must delete a unique row; otherwise, after the
    // first round, the measurement turns into a no-op DELETE. The reserve pool
    // is never fully consumed. All three variants must perform real (non no-op)
    // deletes, so each one deletes from a separate row region; otherwise the
    // second variant deletes the same ids again and measures the "0 rows" path
    // instead of the DELETE cost.
    const std::size_t delete_iterations = std::max<std::size_t>(
        1, std::min<std::size_t>(200, (rows - 8) / (3 * (repetitions + 1))));
    const std::size_t delete_budget =
        delete_iterations * repetitions + delete_iterations / 10 + 8;
    const std::size_t entityfy_delete_offset = rows - 3 * delete_budget;
    const std::size_t raw_cached_delete_offset = rows - 2 * delete_budget;
    const std::size_t raw_prepared_delete_offset = rows - delete_budget;

    std::size_t entityfy_delete_cursor = 0;
    std::size_t raw_cached_delete_cursor = 0;
    std::size_t raw_prepared_delete_cursor = 0;

    const auto entityfy_delete = measure(
        [&](std::size_t) {
            const auto id = entityfy_ids[entityfy_delete_offset + entityfy_delete_cursor++];
            g_sink += entityfy_users.remove_by_id(id);
        },
        delete_iterations,
        repetitions);

    const auto raw_delete_cached = measure(
        [&](std::size_t) {
            const auto id = raw_ids[raw_cached_delete_offset + raw_cached_delete_cursor++];
            g_sink += raw_delete(id, raw_delete_query);
        },
        delete_iterations,
        repetitions);

    const auto raw_delete_prepared = measure(
        [&](std::size_t) {
            statement query{raw_db.get(), "DELETE FROM bench_users WHERE id = ?"};
            const auto id = raw_ids[raw_prepared_delete_offset + raw_prepared_delete_cursor++];
            g_sink += raw_delete(id, query);
        },
        delete_iterations,
        repetitions);

    print_timing_row("remove_by_id", entityfy_delete, raw_delete_cached, raw_delete_prepared, true);

    // Same process, same data: statement cache on/off comparison.
    // Absolute times depend on machine load, so this comparison is more reliable.
    {
        entityfy_db.set_statement_cache_capacity(0);

        const auto pk_without_cache = measure(
            [&](std::size_t index) {
                const auto user = entityfy_users.find(entityfy_ids[order[index % point_iterations]]);
                g_sink += user ? static_cast<std::uint64_t>(user->id) : 0;
            },
            point_iterations,
            repetitions);

        const auto unique_without_cache = measure(
            [&](std::size_t index) {
                const auto& email = emails[order[index % point_iterations]];
                const auto user = entityfy_users.find_one(entityfy::where(&bench_user::email).eq(email));
                g_sink += user ? static_cast<std::uint64_t>(user->id) : 0;
            },
            point_iterations,
            repetitions);

        entityfy_db.set_statement_cache_capacity(32);

        std::cout << std::format("\n{:<26}{:>14}{:>16}{:>12}\n",
                                 "statement cache",
                                 "on ns",
                                 "off ns",
                                 "gain");
        std::cout << std::string(70, '-') << "\n";

        const auto cache_row = [](std::string_view name, const timing& on, const timing& off) {
            const double gain = off.median_ns > 0.0 ? off.median_ns / on.median_ns : 0.0;
            std::cout << std::format("{:<26}{:>14.0f}{:>16.0f}{:>11.2f}x\n",
                                     name,
                                     on.median_ns,
                                     off.median_ns,
                                     gain);
        };

        cache_row("find (primary key)", entityfy_pk, pk_without_cache);
        cache_row("find_one (unique email)", entityfy_unique, unique_without_cache);
    }

    // ---- post-timing correctness ----
    const std::int64_t entityfy_final_count = entityfy_users.count();
    const std::int64_t raw_final_count = raw_count(raw_count_query);

    const std::size_t deleted_per_variant = delete_budget - 8;
    const std::int64_t expected_entityfy_count = static_cast<std::int64_t>(rows - deleted_per_variant);
    const std::int64_t expected_raw_count = static_cast<std::int64_t>(rows - 2 * deleted_per_variant);

    if(entityfy_final_count != expected_entityfy_count || raw_final_count != expected_raw_count)
    {
        std::cerr << "FAIL: post-delete counts differ (entityfy=" << entityfy_final_count
                  << " expected=" << expected_entityfy_count << ", raw=" << raw_final_count
                  << " expected=" << expected_raw_count << ")\n";
        return 1;
    }

    std::cout << std::format("\n[ok] delete verified: entityfy={} raw={} ({} real deletes per variant)\n",
                             entityfy_final_count,
                             raw_final_count,
                             deleted_per_variant);
    std::cout << std::format("[ok] measurement sink (optimization barrier): {}\n", static_cast<std::uint64_t>(g_sink));

    run_context_bench(emails, names, rows, repetitions);

    std::cout << "\nNote: raw cached = a single prepared statement; raw prep-each = prepare/finalize on every call.\n";
    std::cout << "Note: entityfy timings include typed materialization (std::string copies).\n";

    return 0;
}
