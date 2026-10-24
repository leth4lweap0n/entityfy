#include <entityfy/db_context.hpp>

#include <algorithm>
#include <exception>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace entityfy
{
namespace
{

// One in-flight save per context. Save attempts that overlap the holder fail
// instead of queueing behind it. The holder is release-on-scope-exit, unless
// the work moves to the async worker, which then owns the release.
class save_gate
{
public:
    explicit save_gate(std::atomic<bool>& flag)
        : flag_{&flag}
    {
        bool idle = false;

        if(!flag_->compare_exchange_strong(idle, true, std::memory_order_acq_rel))
        {
            throw std::logic_error{"a save is already in progress"};
        }
    }

    ~save_gate()
    {
        release();
    }

    save_gate(const save_gate&) = delete;
    save_gate& operator=(const save_gate&) = delete;

    void release() noexcept
    {
        if(std::atomic<bool>* flag = std::exchange(flag_, nullptr))
        {
            flag->store(false, std::memory_order_release);
        }
    }

    // The gate stays closed, but this handle no longer releases it.
    void disarm() noexcept
    {
        flag_ = nullptr;
    }

private:
    std::atomic<bool>* flag_;
};

bool is_transient(const std::exception& error)
{
    if(dynamic_cast<const entityfy::transient_error*>(&error) != nullptr)
    {
        return true;
    }

    const std::string message = error.what();

    for(const std::string_view needle: {            "database is locked",
            "database table is locked",
            "database schema is locked",
            "deadlock detected",
            "could not serialize access",
            "lock timeout",
        })
    {
        if(message.find(needle) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

} // namespace

db_context::~db_context()
{
    lazy_link_->alive = false;
    drain_native_saves();
}

save_result db_context::save_changes()
{
    const save_gate gate{save_in_progress_};
    const std::lock_guard lock{save_mutex_};
    return save_changes_with_retry();
}

void db_context::set_async_save_strategy(async_save_strategy strategy)
{
    if(strategy == async_save_strategy::native && !native_async_save_available())
    {
        throw std::logic_error{"entityfy: the native asynchronous save path is not available for this "
                               "database or one of the tracked models"};
    }

    async_save_strategy_ = strategy;
}

bool db_context::native_async_save_available() const
{
    if(database_.async_mode() != entityfy::async_capability::native)
    {
        return false;
    }

    return std::ranges::all_of(order_, [](const std::shared_ptr<detail::saveable>& state) {
        return state->supports_native_async_save();
    });
}

void db_context::drain_native_saves()
{
    std::unique_lock lock{native_save_mutex_};
    native_save_cv_.wait(lock, [this] { return native_saves_in_flight_ == 0; });
}

entityfy::task<save_result> db_context::save_changes_once_async()
{
    save_gate gate{save_in_progress_};

    if(!native_async_save_available())
    {
        throw std::logic_error{"entityfy: the native asynchronous save path is not available for this "
                               "database or one of the tracked models"};
    }

    {
        const std::lock_guard lock{native_save_mutex_};
        ++native_saves_in_flight_;
    }

    last_save_used_native_async_.store(true, std::memory_order_release);

    try
    {
        // The native path releases the gate after its last context access,
        // just before it completes the returned task.
        auto pending = save_changes_once_async_native();
        gate.disarm();
        return pending;
    }
    catch(...)
    {
        {
            const std::lock_guard lock{native_save_mutex_};
            --native_saves_in_flight_;
        }
        native_save_cv_.notify_all();
        throw;
    }
}

entityfy::task<save_result> db_context::save_changes_async()
{
    const bool use_native = async_save_strategy_ != async_save_strategy::offload &&
                            native_async_save_available();

    if(async_save_strategy_ == async_save_strategy::native && !use_native)
    {
        throw std::logic_error{"entityfy: the native asynchronous save path is not available for this "
                               "database or one of the tracked models"};
    }

    if(use_native)
    {
        return save_changes_once_async();
    }

    save_gate gate{save_in_progress_};
    last_save_used_native_async_.store(false, std::memory_order_release);

    auto pair = entityfy::make_task<save_result>();
    auto completer = std::make_shared<entityfy::task_completer<save_result>>(std::move(pair.completer));

    // The job owns the gate only once submit accepted it. Until then the gate
    // handle below releases on every exit path, including a failed submit.
    save_executor().submit([this, completer] {
        std::exception_ptr error;
        save_result        result;

        try
        {
            const std::lock_guard lock{save_mutex_};
            result = save_changes_with_retry();
        }
        catch(...)
        {
            error = std::current_exception();
        }

        // Free the gate before publishing, so a caller that observes
        // completion can start the next save immediately.
        save_in_progress_.store(false, std::memory_order_release);

        if(error)
        {
            completer->set_exception(std::move(error));
        }
        else
        {
            completer->set_value(std::move(result));
        }
    });

    gate.disarm();

    return std::move(pair.value);
}

entityfy::thread_pool_executor& db_context::save_executor()
{
    const std::lock_guard lock{executor_mutex_};

    if(!save_executor_)
    {
        save_executor_ = std::make_unique<entityfy::thread_pool_executor>(1, 16);
    }

    return *save_executor_;
}

save_result db_context::save_changes_with_retry()
{
    const int attempts = retry_.attempts < 1 ? 1 : retry_.attempts;

    for(int attempt = 1;; ++attempt)
    {
        try
        {
            auto result = save_changes_once();
            result.attempts = static_cast<std::size_t>(attempt);
            return result;
        }
        catch(const std::exception& error)
        {
            if(attempt >= attempts || !is_transient(error))
            {
                throw;
            }

            std::this_thread::sleep_for(retry_.delay * attempt);
        }
    }
}

void db_context::clear()
{
    // Handles bound to the previous state must not dereference it through a
    // stale owner pointer after the sets are gone.
    ++lazy_link_->generation;

    order_.clear();
    states_.clear();
}

void db_context::migrate_all()
{
    // Parents first: a table can only be created after every table it
    // references exists.
    std::vector<std::size_t> remaining(models_.size());
    for(std::size_t index = 0; index < models_.size(); ++index)
    {
        remaining[index] = index;
    }

    std::set<std::string> resolved;
    std::vector<std::size_t> ordered;

    while(!remaining.empty())
    {
        bool progress = false;

        for(auto iterator = remaining.begin(); iterator != remaining.end();)
        {
            const auto& model = models_[*iterator];
            bool ready = true;

            for(const auto& dependency: model.foreign_keys)
            {
                if(dependency == model.table)
                {
                    continue;
                }

                const bool pending = std::ranges::any_of(models_, [&](const model_info& candidate) {
                    return candidate.table == dependency;
                });

                if(pending && !resolved.contains(dependency))
                {
                    ready = false;
                    break;
                }
            }

            if(ready)
            {
                ordered.push_back(*iterator);
                resolved.insert(model.table);
                iterator = remaining.erase(iterator);
                progress = true;
            }
            else
            {
                ++iterator;
            }
        }

        if(!progress)
        {
            throw std::runtime_error{"entityfy: circular foreign key dependency between the registered models"};
        }
    }

    for(const auto index: ordered)
    {
        migrators_[index]();
    }
}

std::vector<std::shared_ptr<detail::saveable>> db_context::save_order()
{
    std::vector<std::shared_ptr<detail::saveable>> ordered;
    std::set<std::string>                          resolved;
    std::vector<std::shared_ptr<detail::saveable>> remaining = order_;

    while(!remaining.empty())
    {
        bool progress = false;

        for(auto iterator = remaining.begin(); iterator != remaining.end();)
        {
            bool ready = true;

            for(const auto& dependency: (*iterator)->referenced_tables())
            {
                if(dependency == (*iterator)->table_name() || resolved.contains(dependency))
                {
                    continue;
                }

                const bool pending_dependency = std::ranges::any_of(remaining, [&](const auto& other) {
                    return other->table_name() == dependency;
                });

                if(pending_dependency)
                {
                    ready = false;
                    break;
                }
            }

            if(ready)
            {
                ordered.push_back(*iterator);
                resolved.insert((*iterator)->table_name());
                iterator = remaining.erase(iterator);
                progress = true;
            }
            else
            {
                ++iterator;
            }
        }

        if(!progress)
        {
            for(auto& state: remaining)
            {
                ordered.push_back(state);
            }

            break;
        }
    }

    return ordered;
}

save_result db_context::save_changes_once()
{
    save_result total;

    auto ordered = save_order();

    for(auto& state: order_)
    {
        state->begin_save();
    }

    try
    {
        database_.transaction([&] {
            for(auto iterator = ordered.rbegin(); iterator != ordered.rend(); ++iterator)
            {
                total += (*iterator)->delete_removed(*this);
            }

            for(auto& state: ordered)
            {
                total += state->save(*this);
            }
        });

        for(auto& state: order_)
        {
            state->commit_save();
        }
    }
    catch(...)
    {
        for(auto& state: order_)
        {
            state->rollback_save();
        }

        throw;
    }

    return total;
}


void db_context::finish_native_save() noexcept
{
    save_in_progress_.store(false, std::memory_order_release);

    {
        const std::lock_guard lock{native_save_mutex_};
        --native_saves_in_flight_;
    }

    native_save_cv_.notify_all();
}

entityfy::task<save_result> db_context::save_changes_once_async_native()
{
    auto ordered = save_order();

    for(auto& state: order_)
    {
        state->begin_save();
    }

    std::vector<detail::async_save_chain::step> steps;
    steps.reserve(ordered.size() * 2 + 2);

    steps.emplace_back([this] {
        return database_.execute_async(entityfy::statement{.sql = "BEGIN"}).then([](auto) {
            return save_result{};
        });
    });

    for(auto iterator = ordered.rbegin(); iterator != ordered.rend(); ++iterator)
    {
        auto state = *iterator;
        steps.emplace_back([this, state] { return state->delete_removed_async(*this); });
    }

    for(auto& state: ordered)
    {
        steps.emplace_back([this, state] { return state->save_async(*this); });
    }

    steps.emplace_back([this] {
        return database_.execute_async(entityfy::statement{.sql = "COMMIT"}).then([](auto) {
            return save_result{};
        });
    });

    auto pair = entityfy::make_task<save_result>();
    auto completer = std::make_shared<entityfy::task_completer<save_result>>(std::move(pair.completer));

    try
    {
        auto work = std::make_shared<entityfy::task<save_result>>(detail::async_save_chain::run(std::move(steps)));

        work->on_complete([this, work, completer]() noexcept {
            save_result      total;
            std::exception_ptr error;

            try
            {
                total = work->get();
            }
            catch(...)
            {
                error = std::current_exception();
            }

            if(error == nullptr)
            {
                for(auto& state: order_)
                {
                    state->commit_save();
                }

                finish_native_save();
                completer->set_value(std::move(total));
                return;
            }

            auto rollback = [this, completer, error] {
                for(auto& state: order_)
                {
                    state->rollback_save();
                }

                finish_native_save();
                completer->set_exception(error);
            };

            try
            {
                auto pending_rollback =
                    std::make_shared<entityfy::task<entityfy::detail::execution_result>>(
                        database_.execute_async(entityfy::statement{.sql = "ROLLBACK"}));

                pending_rollback->on_complete([pending_rollback, rollback]() noexcept {
                    try
                    {
                        (void)pending_rollback->get();
                    }
                    catch(...)
                    {
                    }

                    rollback();
                });
            }
            catch(...)
            {
                rollback();
            }
        });
    }
    catch(...)
    {
        for(auto& state: order_)
        {
            state->rollback_save();
        }

        finish_native_save();
        throw;
    }

    return std::move(pair.value);
}

} // namespace entityfy
