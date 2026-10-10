#pragma once

#include <entityfy/entityfy.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace entityfy
{

struct save_result
{
    std::size_t inserted = 0;
    std::size_t updated = 0;
    std::size_t removed = 0;
    std::size_t attempts = 1;

    save_result& operator+=(const save_result& other) noexcept
    {
        inserted += other.inserted;
        updated += other.updated;
        removed += other.removed;
        return *this;
    }
};

enum class entity_state
{
    unchanged,
    added,
    modified,
    removed,
    detached,
};

struct retry_policy
{
    int                       attempts = 1;
    std::chrono::milliseconds delay{50};
};

template <typename T>
class entity_set;

class context;

namespace detail
{

struct saveable
{
    virtual ~saveable() = default;

    virtual void begin_save() = 0;
    virtual save_result delete_removed(context& owner) = 0;
    virtual save_result save(context& owner) = 0;
    virtual void commit_save() noexcept = 0;
    virtual void rollback_save() = 0;

    [[nodiscard]] virtual std::string table_name() const = 0;
    [[nodiscard]] virtual std::vector<std::string> referenced_tables() const = 0;
};

} // namespace detail

class context
{
public:
    explicit context(std::string uri)
        : database_{std::move(uri)}
    {
        if(database_.target_dialect() == entityfy::dialect::sqlite)
        {
            execute("PRAGMA foreign_keys = ON");
        }
    }

    context(const context&) = delete;
    context& operator=(const context&) = delete;

    template <typename T>
    [[nodiscard]] entity_set<T> set();

    // Applies all tracked changes inside a single transaction.
    save_result save_changes();

    // Runs the same operation on a worker thread. Saves are serialized by
    // an internal mutex; the context cannot be used concurrently from
    // other threads (same rule as EF Core).
    [[nodiscard]] std::future<save_result> save_changes_async();

    // Drops the tracked entities and the identity map; the DB connection is kept.
    void clear();

    template <typename T>
    void migrate()
    {
        database_.template table<T>().migrate();
    }

    template <typename T>
    void migrate_force()
    {
        database_.template table<T>().migrate_force();
    }

    template <typename T>
    void reset_schema()
    {
        database_.template table<T>().reset_schema();
    }

    [[nodiscard]] std::uint64_t execute(std::string_view sql)
    {
        return execute(sql, {});
    }

    [[nodiscard]] std::uint64_t execute(std::string_view sql, const std::vector<entityfy::sql_value>& binds)
    {
        log(sql);
        return database_.execute(entityfy::statement{.sql = std::string{sql}, .binds = binds}).rows_affected;
    }

    [[nodiscard]] std::vector<std::vector<entityfy::sql_value>> query(std::string_view sql)
    {
        return query(sql, {});
    }

    [[nodiscard]] std::vector<std::vector<entityfy::sql_value>> query(
        std::string_view sql,
        const std::vector<entityfy::sql_value>& binds)
    {
        log(sql);
        return database_.query(entityfy::statement{.sql = std::string{sql}, .binds = binds});
    }

    [[nodiscard]] entityfy::client& db() noexcept
    {
        return database_;
    }

    template <typename Function>
    decltype(auto) transaction(Function&& function)
    {
        return database_.transaction(std::forward<Function>(function));
    }

    void set_logger(std::function<void(std::string_view)> logger)
    {
        logger_ = std::move(logger);
    }

    void set_retry_policy(retry_policy value)
    {
        retry_ = value;
    }

    void set_batch_parameter_limit(std::size_t value)
    {
        batch_parameter_limit_ = value == 0 ? 1 : value;
    }

    [[nodiscard]] std::size_t batch_parameter_limit() const noexcept
    {
        return batch_parameter_limit_;
    }

    void log(std::string_view sql) const
    {
        if(logger_)
        {
            logger_(sql);
        }
    }

private:
    [[nodiscard]] save_result save_changes_once();
    [[nodiscard]] save_result save_changes_with_retry();

    entityfy::client                                        database_;
    std::unordered_map<std::type_index, std::shared_ptr<detail::saveable>> states_;
    std::vector<std::shared_ptr<detail::saveable>>         order_;
    std::function<void(std::string_view)>                  logger_;
    retry_policy                                           retry_{};
    std::size_t                                            batch_parameter_limit_ = 1000;
    std::mutex                                             save_mutex_;
};

} // namespace entityfy

#include <entityfy/entity_set.hpp>
