#pragma once

#include <entityfy/entityfy.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
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

    [[nodiscard]] std::size_t total() const noexcept
    {
        return inserted + updated + removed;
    }
};

// Metadata of one entity type known to a context. Every field is derived
// through reflection, so a context never repeats schema information.
struct model_info
{
    std::string              table{};
    std::vector<std::string> columns{};
    // Tables this entity points at through a foreign key.
    std::vector<std::string> foreign_keys{};
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

// EF Core parity: a context owns one DbSet per entity type. The set itself is
// a lazy handle, so it can be declared as a member of a user defined context:
//
//     struct app_context : entityfy::db_context
//     {
//         using entityfy::db_context::db_context;
//         entityfy::db_set<user> Users{*this};
//         entityfy::db_set<post> Posts{*this};
//     };
//
//     app_context ctx{"sqlite://app.db"};
//     ctx.Users.add(user{.email = "ada@example.com"});
//     ctx.save_changes();
template <typename T>
class db_set;

class db_context;

namespace detail
{

struct saveable
{
    virtual ~saveable() = default;

    virtual void begin_save() = 0;
    virtual save_result delete_removed(db_context& owner) = 0;
    virtual save_result save(db_context& owner) = 0;
    virtual void commit_save() noexcept = 0;
    virtual void rollback_save() = 0;

    [[nodiscard]] virtual std::string table_name() const = 0;
    [[nodiscard]] virtual std::vector<std::string> referenced_tables() const = 0;
};

template <typename T>
struct set_state;

} // namespace detail

// The DbContext equivalent: owns the connection, the change tracker and the
// per entity type tracking state.
class db_context
{
public:
    explicit db_context(std::string uri)
        : database_{std::move(uri)}
    {
        if(database_.target_dialect() == entityfy::dialect::sqlite)
        {
            execute("PRAGMA foreign_keys = ON");
        }
    }

    db_context(const db_context&) = delete;
    db_context& operator=(const db_context&) = delete;

    // Generic accessor for a tracked entity set. Typed sets declared as
    // context members are the preferred form.
    template <typename T>
    [[nodiscard]] db_set<T> set();

    // Metadata of every entity type this context knows about. The list is
    // filled through reflection when a set is first used.
    [[nodiscard]] const std::vector<model_info>& model() const noexcept
    {
        return models_;
    }

    // Creates the tables of every known entity type, parents first. The order
    // is derived from the foreign key graph, so it needs no manual listing.
    void migrate_all();

    // Applies all tracked changes inside a single transaction.
    save_result save_changes();

    // Runs the same operation on a worker thread. Saves are serialized by
    // an internal mutex; the context cannot be used concurrently from
    // other threads (same rule as EF Core).
    [[nodiscard]] std::future<save_result> save_changes_async();

    // Drops the tracked entities and the identity map; the DB connection is kept.
    void clear();

    // Tracks an entity in the set that belongs to its type.
    template <typename T>
    T& add(T entity)
    {
        return set<T>().add(std::move(entity));
    }

    // Marks a tracked entity as removed.
    template <typename T>
    void remove(const T& entity)
    {
        set<T>().remove(entity);
    }

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

    // Escape hatch to the lower level client API.
    [[nodiscard]] entityfy::client& database() noexcept
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
    template <typename T>
    friend class db_set;

    // Creates the tracking state for T on first use and returns the shared
    // state, so every db_set<T> of this context observes the same identity map.
    template <typename T>
    [[nodiscard]] std::shared_ptr<detail::set_state<T>> state_for();

    // Records the reflection derived metadata of T once.
    template <typename T>
    void register_model();

    [[nodiscard]] save_result save_changes_once();
    [[nodiscard]] save_result save_changes_with_retry();

    entityfy::client                                        database_;
    std::unordered_map<std::type_index, std::shared_ptr<detail::saveable>> states_;
    std::vector<std::shared_ptr<detail::saveable>>         order_;
    std::vector<model_info>                                models_;
    std::unordered_map<std::type_index, std::size_t>        model_index_;
    std::vector<std::function<void()>>                     migrators_;
    std::function<void(std::string_view)>                  logger_;
    retry_policy                                           retry_{};
    std::size_t                                            batch_parameter_limit_ = 1000;
    std::mutex                                             save_mutex_;
};

} // namespace entityfy

#include <entityfy/db_set.hpp>
