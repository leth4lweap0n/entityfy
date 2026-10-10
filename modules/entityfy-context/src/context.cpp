#include <entityfy/context.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace entityfy
{
namespace
{

bool is_transient(const std::exception& error)
{
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

save_result context::save_changes()
{
    const std::lock_guard lock{save_mutex_};
    return save_changes_with_retry();
}

std::future<save_result> context::save_changes_async()
{
    return std::async(std::launch::async, [this] {
        const std::lock_guard lock{save_mutex_};
        return save_changes_with_retry();
    });
}

save_result context::save_changes_with_retry()
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

void context::clear()
{
    order_.clear();
    states_.clear();
}

save_result context::save_changes_once()
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
