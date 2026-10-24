#pragma once

#include <entityfy/entityfy.hpp>
#include <entityfy/executor.hpp>
#include <entityfy/lazy.hpp>
#include <entityfy/task.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
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

// How save_changes_async() reaches the database. `automatic` prefers the native
// path whenever the backend and every tracked model support it, and falls back
// to a worker-thread offload otherwise. `offload` always uses the executor and
// keeps the historical behaviour; `native` refuses to start when the native
// path is unavailable instead of quietly degrading.
enum class async_save_strategy
{
    automatic,
    offload,
    native,
};

// EF Core parity: a context owns one db_set per entity type. The set itself is
// a lazy handle, so it can be declared as a member of a user defined context:
//
//     struct app_context : entityfy::db_context
//     {
//         using entityfy::db_context::db_context;
//         entityfy::db_set<user> users{*this};
//         entityfy::db_set<post> posts{*this};
//     };
//
//     app_context ctx{"sqlite://app.db"};
//     ctx.users.add(user{.email = "ada@example.com"});
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

    // Native asynchronous save. A state reports the capability only when its
    // model can be written without a synchronous round trip: no generated
    // primary key to read back and no persisted concurrency token to reload.
    // The defaults keep every other model on the offload path; they throw so a
    // capability that was not reported can never be reached silently.
    [[nodiscard]] virtual bool supports_native_async_save() const noexcept
    {
        return false;
    }

    virtual entityfy::task<save_result> delete_removed_async(db_context& owner)
    {
        (void)owner;
        throw std::logic_error{"entityfy: native async delete is not supported for this model"};
    }

    virtual entityfy::task<save_result> save_async(db_context& owner)
    {
        (void)owner;
        throw std::logic_error{"entityfy: native async save is not supported for this model"};
    }
};

// One step of an asynchronous save. The step builds its statement when it runs,
// not when the step is listed, so a later statement is only prepared after the
// previous one resolved and the row counts it produced are known.
using async_save_step = std::function<entityfy::task<save_result>()>;

// Runs a list of save steps one after another on the thread that completes each
// one and accumulates their results; the first failure ends the sequence. The
// steps run lazily, so a caller can describe work that depends on what the
// database already accepted. A step that completes synchronously is continued in
// a loop instead of recursing, so a chain over thousands of rows keeps a flat
// stack. The completion callback runs exactly once and never unwinds through the
// thread that completed the last step.
class async_save_chain final : public std::enable_shared_from_this<async_save_chain>
{
public:
    using step = async_save_step;
    using completion = std::function<void(save_result, std::exception_ptr)>;

    async_save_chain(save_result initial, std::vector<step> steps, completion done)
        : total_{initial}
        , steps_{std::move(steps)}
        , done_{std::move(done)}
    {
    }

    async_save_chain(const async_save_chain&) = delete;
    async_save_chain& operator=(const async_save_chain&) = delete;

    // A task that is already completed with `result`, so a step with nothing to
    // do still returns a task.
    [[nodiscard]] static entityfy::task<save_result> completed(save_result result)
    {
        auto pair = entityfy::make_task<save_result>();
        pair.completer.set_value(std::move(result));
        return std::move(pair.value);
    }

    // Runs `steps` and returns a task with their accumulated result, or with the
    // first failure. The chain starts before this returns, so the task may
    // already be ready.
    [[nodiscard]] static entityfy::task<save_result> run(std::vector<step> steps)
    {
        auto pair = entityfy::make_task<save_result>();
        auto completer = std::make_shared<entityfy::task_completer<save_result>>(std::move(pair.completer));

        auto chain = std::make_shared<async_save_chain>(
            save_result{},
            std::move(steps),
            [completer](save_result total, std::exception_ptr error) {
                if(error != nullptr)
                {
                    completer->set_exception(std::move(error));
                    return;
                }

                completer->set_value(std::move(total));
            }
        );

        chain->start();

        return std::move(pair.value);
    }

    // Starts the chain. Never throws: the outcome is delivered through the
    // completion callback, because a throw from here would escape through the
    // database thread that completed the previous step.
    void start() noexcept
    {
        advance();
    }

private:
    void advance() noexcept
    {
        if(running_)
        {
            // A step finished synchronously; let the outer loop carry on instead
            // of growing the stack by one frame per step.
            resume_ = true;
            return;
        }

        running_ = true;

        for(;;)
        {
            resume_ = false;

            if(finished_)
            {
                running_ = false;
                return;
            }

            if(cursor_ >= steps_.size())
            {
                running_ = false;
                complete();
                return;
            }

            try
            {
                entityfy::task<save_result> pending = steps_[cursor_++]();

                if(!pending.valid())
                {
                    throw std::logic_error{"entityfy: an asynchronous save step returned no task"};
                }

                auto handle = std::make_shared<entityfy::task<save_result>>(std::move(pending));
                auto self = shared_from_this();

                // The continuation reads the result through get(), which is safe
                // here because the task is already complete when it runs.
                handle->on_complete([self, handle]() noexcept {
                    try
                    {
                        self->total_ += handle->get();
                    }
                    catch(...)
                    {
                        self->fail(std::current_exception());
                        return;
                    }

                    self->advance();
                });
            }
            catch(...)
            {
                running_ = false;
                fail(std::current_exception());
                return;
            }

            if(finished_ || !resume_)
            {
                running_ = false;
                return;
            }
        }
    }

    void complete() noexcept
    {
        finish_once(total_, nullptr);
    }

    void fail(std::exception_ptr error) noexcept
    {
        finish_once(total_, std::move(error));
    }

    void finish_once(save_result total, std::exception_ptr error) noexcept
    {
        if(finished_)
        {
            return;
        }

        finished_ = true;

        if(!done_)
        {
            return;
        }

        try
        {
            done_(std::move(total), std::move(error));
        }
        catch(...)
        {
            // No exception may unwind through the thread that completed the last
            // step; the process would terminate.
        }
    }

    save_result       total_{};
    std::vector<step> steps_{};
    completion        done_{};
    std::size_t       cursor_ = 0;
    bool              running_ = false;
    bool              resume_ = false;
    bool              finished_ = false;
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
        , lazy_link_{std::make_shared<detail::lazy_link>()}
    {
        lazy_link_->owner = this;

        if(database_.target_dialect() == entityfy::dialect::sqlite)
        {
            execute("PRAGMA foreign_keys = ON");
        }
    }

    db_context(const db_context&) = delete;
    db_context& operator=(const db_context&) = delete;

    // Marks the binding state dead, so a lazy handle that outlives the context
    // reports context_destroyed instead of following the dangling owner.
    ~db_context();

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

    // Runs the same operation asynchronously. One save per context at a time: a
    // save that overlaps this one throws std::logic_error instead of being
    // queued.
    //
    // Two strategies exist (see set_async_save_strategy): the default
    // `automatic` drives the statements through the backend's native
    // asynchronous surface (`execute_async`) when the database reports native
    // capability and every tracked model supports it, and otherwise falls back
    // to a lazily created single-worker executor. The offload path is a thread
    // offload, the native path performs no blocking statement on the calling
    // thread. last_save_used_native_async() reports which one the last completed
    // save took.
    //
    // The native path does not retry. retry_policy keeps applying to the
    // synchronous save_changes() and to the offload path; a native save that
    // fails publishes the failure unchanged (after rolling the transaction back
    // and restoring the tracker), so a caller that wants retries selects
    // async_save_strategy::offload explicitly.
    //
    // Precondition: until the returned task completes, the tracked sets must not
    // be mutated and the context must not be used from another thread; the save
    // reads the tracker and the connection. The task cannot be stopped:
    // request_cancel() is observed but no cancel hook is installed, so the save
    // still runs to completion and get() returns its result. A continuation runs
    // on the completing thread, so it must not release the last owner of the
    // context (see the lifetime rule in entityfy/task.hpp); destroying the
    // context from another thread waits for the native save to finish first.
    [[nodiscard]] entityfy::task<save_result> save_changes_async();

    // One asynchronous save attempt. This is the native one-shot: it throws
    // std::logic_error unless the database reports native async support and
    // every tracked model is native-capable, and it never retries. Use
    // save_changes_async() for the strategy/fallback/retry-compatible entry
    // point.
    [[nodiscard]] entityfy::task<save_result> save_changes_once_async();

    // Picks how save_changes_async() reaches the database. `native` throws
    // std::logic_error when the current database or a tracked model cannot run
    // the native path; `automatic` never throws and falls back to the executor.
    void set_async_save_strategy(async_save_strategy strategy);

    [[nodiscard]] async_save_strategy current_async_save_strategy() const noexcept
    {
        return async_save_strategy_;
    }

    // True when the last completed save_changes_async() used the native path.
    [[nodiscard]] bool last_save_used_native_async() const noexcept
    {
        return last_save_used_native_async_.load(std::memory_order_acquire);
    }

    // True when save_changes_async() would pick the native path right now, for
    // the strategy and the models that are tracked at this moment.
    [[nodiscard]] bool native_async_save_available() const;

    // Drops the tracked entities and the identity map; the DB connection is kept.
    // Handles bound to the dropped state report context_cleared afterwards.
    void clear();

    // Binding state shared with every lazy handle this context binds. Handles
    // keep a weak reference, so a destroyed or cleared context is detected
    // instead of dereferenced.
    [[nodiscard]] std::shared_ptr<detail::lazy_link> lazy_link() const noexcept
    {
        return lazy_link_;
    }

    [[nodiscard]] std::uint64_t lazy_generation() const noexcept
    {
        return lazy_link_->generation;
    }

    // Change detection strategy. With auto detection disabled, save_changes()
    // compares only the entities passed to db_set::mark_modified(); everything
    // else is skipped, which turns a one-row update from O(tracked entities)
    // into O(1). The trade-off is explicit: unmarked edits are not written.
    void set_auto_detect(bool enabled) noexcept
    {
        auto_detect_ = enabled;
    }

    [[nodiscard]] bool auto_detect() const noexcept
    {
        return auto_detect_;
    }

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
    [[nodiscard]] entityfy::task<save_result> save_changes_once_async_native();

    // Creates the single worker that runs async saves on first use.
    [[nodiscard]] entityfy::thread_pool_executor& save_executor();

    // States in the order a save has to write them: parents before children,
    // shared by the synchronous and the native asynchronous path so the two can
    // never drift apart.
    [[nodiscard]] std::vector<std::shared_ptr<detail::saveable>> save_order();

    // Blocks until no native asynchronous save is in flight.
    void drain_native_saves();

    // Releases the save gate and the native in-flight counter exactly once,
    // after the last context access of a native save.
    void finish_native_save() noexcept;

    entityfy::client                                        database_;
    // Shared with the lazy handles bound by this context; never reassigned.
    std::shared_ptr<detail::lazy_link>                      lazy_link_;
    bool                                                    auto_detect_ = true;
    async_save_strategy                                     async_save_strategy_ = async_save_strategy::automatic;
    std::unordered_map<std::type_index, std::shared_ptr<detail::saveable>> states_;
    std::vector<std::shared_ptr<detail::saveable>>         order_;
    std::vector<model_info>                                models_;
    std::unordered_map<std::type_index, std::size_t>        model_index_;
    std::vector<std::function<void()>>                     migrators_;
    std::function<void(std::string_view)>                  logger_;
    retry_policy                                           retry_{};
    std::size_t                                            batch_parameter_limit_ = 1000;
    std::mutex                                             save_mutex_;
    std::mutex                                             executor_mutex_;
    std::atomic<bool>                                      save_in_progress_{false};
    std::atomic<bool>                                      last_save_used_native_async_{false};
    // Guards the drain state below. A native save increments the counter before
    // it touches any member and decrements it after its last access and before
    // it completes the user facing task, so a continuation that observes
    // completion may destroy the context: the destructor waits on the counter
    // and finds it already zero.
    std::mutex                                             native_save_mutex_;
    std::condition_variable                                native_save_cv_;
    std::size_t                                            native_saves_in_flight_ = 0;
    // Declared last on purpose: members are destroyed in reverse order, so the
    // executor joins its worker (draining an in-flight save) before the sets,
    // the connection and the gate are destroyed.
    std::unique_ptr<entityfy::thread_pool_executor>        save_executor_;
};

} // namespace entityfy

#include <entityfy/db_set.hpp>
