#pragma once

#include <entityfy/context.hpp>

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace entityfy
{

// Similar to DbContextPool: reuses ready contexts and returns them to the pool
// with the change tracker cleared when a lease ends. The pool outlives its leases.
class context_pool
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

        [[nodiscard]] context& operator*() const noexcept
        {
            return *context_;
        }

        [[nodiscard]] context* operator->() const noexcept
        {
            return context_.get();
        }

        [[nodiscard]] context* get() const noexcept
        {
            return context_.get();
        }

    private:
        friend class context_pool;

        lease(context_pool* pool, std::unique_ptr<context> context)
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

        context_pool*            pool_;
        std::unique_ptr<context> context_;
    };

    explicit context_pool(std::string uri, std::size_t max_size = 16)
        : uri_{std::move(uri)}, max_size_{max_size == 0 ? 1 : max_size}
    {
    }

    context_pool(const context_pool&) = delete;
    context_pool& operator=(const context_pool&) = delete;

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

        auto created = std::make_unique<context>(uri_);
        ++created_;
        return lease{this, std::move(created)};
    }

    [[nodiscard]] std::size_t idle_count() const
    {
        std::lock_guard lock{mutex_};
        return idle_.size();
    }

    [[nodiscard]] std::size_t created_count() const
    {
        std::lock_guard lock{mutex_};
        return created_;
    }

private:
    void release(std::unique_ptr<context> context) noexcept
    {
        context->clear();
        context->set_logger(nullptr);
        context->set_retry_policy(retry_policy{});
        context->set_batch_parameter_limit(1000);

        {
            std::lock_guard lock{mutex_};
            idle_.push_back(std::move(context));
        }

        available_.notify_one();
    }

    std::string                              uri_;
    std::size_t                              max_size_;
    mutable std::mutex                       mutex_;
    std::condition_variable                  available_;
    std::vector<std::unique_ptr<context>>    idle_;
    std::size_t                              created_ = 0;
};

} // namespace entityfy
