#pragma once

#include <entityfy/db_context.hpp>
#include <entityfy/executor.hpp>
#include <entityfy/task.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace entityfy
{

// Similar to DbContextPool: reuses ready contexts and returns them to the pool
// with the change tracker cleared when a lease ends. The pool outlives its leases.
class db_context_pool
{
public:
    class lease
    {
    public:
        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;

        lease(lease&& other) noexcept
            : pool_{other.pool_}, context_{std::move(other.context_)}
        {
            other.pool_ = nullptr;
        }

        lease& operator=(lease&& other) noexcept
        {
            if(this != &other)
            {
                release();
                pool_ = other.pool_;
                context_ = std::move(other.context_);
                other.pool_ = nullptr;
            }

            return *this;
        }

        ~lease()
        {
            release();
        }

        [[nodiscard]] db_context& operator*() const noexcept
        {
            return *context_;
        }

        [[nodiscard]] db_context* operator->() const noexcept
        {
            return context_.get();
        }

        [[nodiscard]] db_context* get() const noexcept
        {
            return context_.get();
        }

    private:
        friend class db_context_pool;

        lease(db_context_pool* pool, std::unique_ptr<db_context> context)
            : pool_{pool}, context_{std::move(context)}
        {
        }

        void release() noexcept
        {
            if(pool_ != nullptr && context_ != nullptr)
            {
                pool_->release(std::move(context_));
                pool_ = nullptr;
            }
        }

        db_context_pool*            pool_;
        std::unique_ptr<db_context> context_;
    };

    explicit db_context_pool(std::string uri, std::size_t max_size = 16)
        : uri_{std::move(uri)}, max_size_{max_size == 0 ? 1 : max_size}
    {
    }

    db_context_pool(const db_context_pool&) = delete;
    db_context_pool& operator=(const db_context_pool&) = delete;

    [[nodiscard]] lease acquire()
    {
        std::unique_lock lock{mutex_};

        if(idle_.empty() && created_ >= max_size_)
        {
            available_.wait(lock, [&] { return !idle_.empty(); });
        }

        if(!idle_.empty())
        {
            auto context = std::move(idle_.back());
            idle_.pop_back();
            return lease{this, std::move(context)};
        }

        auto created = std::make_unique<db_context>(uri_);
        ++created_;
        return lease{this, std::move(created)};
    }

    // Acquires on a lazily created executor of max_size() workers; each job
    // calls the blocking acquire(), so this is a thread offload, not native
    // I/O asynchrony. A lease must not outlive the pool (as with acquire()).
    //
    // request_cancel() takes effect only before the job starts: the job checks
    // a shared flag before touching the pool and completes with set_cancelled()
    // instead. A started acquire is not interrupted, so its task still produces
    // a lease even when the cancel request arrived meanwhile. A continuation
    // runs on a pool worker and must not release the last owner of the pool
    // (see the lifetime rule in entityfy/task.hpp).
    [[nodiscard]] entityfy::task<lease> acquire_async()
    {
        auto pair = entityfy::make_task<lease>();
        auto completer = std::make_shared<entityfy::task_completer<lease>>(std::move(pair.completer));
        auto cancelled = std::make_shared<std::atomic<bool>>(false);

        completer->set_cancel_hook([cancelled] { cancelled->store(true, std::memory_order_release); });

        acquire_executor().submit([this, completer, cancelled] {
            if(cancelled->load(std::memory_order_acquire))
            {
                completer->set_cancelled();
                return;
            }

            std::exception_ptr   error;
            std::optional<lease> acquired;

            try
            {
                acquired.emplace(acquire());
            }
            catch(...)
            {
                error = std::current_exception();
            }

            if(error)
            {
                completer->set_exception(std::move(error));
            }
            else
            {
                completer->set_value(std::move(*acquired));
            }
        });

        return std::move(pair.value);
    }

    [[nodiscard]] std::size_t idle_count() const
    {
        std::unique_lock lock{mutex_};
        return idle_.size();
    }

    [[nodiscard]] std::size_t created_count() const
    {
        std::unique_lock lock{mutex_};
        return created_;
    }

private:
    void release(std::unique_ptr<db_context> context) noexcept
    {
        if(!context)
        {
            return;
        }

        context->clear();

        std::unique_lock lock{mutex_};
        idle_.push_back(std::move(context));
        available_.notify_one();
    }

    // Creates the worker pool that runs async acquires on first use.
    [[nodiscard]] entityfy::thread_pool_executor& acquire_executor()
    {
        const std::lock_guard lock{executor_mutex_};

        if(!acquire_executor_)
        {
            acquire_executor_ = std::make_unique<entityfy::thread_pool_executor>(max_size_, max_size_ * 2);
        }

        return *acquire_executor_;
    }

    std::string                              uri_;
    std::size_t                              max_size_;
    mutable std::mutex                       mutex_;
    std::condition_variable                  available_;
    std::vector<std::unique_ptr<db_context>> idle_;
    std::size_t                              created_ = 0;
    std::mutex                               executor_mutex_;
    // Declared last on purpose: members are destroyed in reverse order, so the
    // executor joins its workers (draining a pending or in-flight acquire)
    // before the idle contexts, the mutex and the condition variable it uses
    // are destroyed.
    std::unique_ptr<entityfy::thread_pool_executor> acquire_executor_;
};

} // namespace entityfy
