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
}

save_result db_context::save_changes()
{
    const save_gate gate{save_in_progress_};
    const std::lock_guard lock{save_mutex_};
    return save_changes_with_retry();
}

entityfy::task<save_result> db_context::save_changes_async()
{
    save_gate gate{save_in_progress_};

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

save_result db_context::save_changes_once()
{
    save_result total;

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

} // namespace entityfy
